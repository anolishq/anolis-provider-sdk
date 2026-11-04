// anolis-provider-reference: the SDK's conformance fixture and a worked example.
//
// Each configured device is a simulated heater with a first-order thermal model,
// the same device the Python SDK's reference provider and the provider templates
// use: its temperature approaches ambient plus the rise its power level gives,
// with a fixed time constant, and it counts the energy it draws. No hardware.
//
// It shows what a provider built on this SDK supplies: a config schema declared
// once (for --config-schema and --check-config), a ProviderRuntime answering
// inventory, capabilities, reads and calls, the executable-profile flags, and an
// on_shutdown hook that puts every output in its safe state.

#include <yaml-cpp/yaml.h>

#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "anolis/provider_sdk/config.hpp"
#include "anolis/provider_sdk/config_validate.hpp"
#include "anolis/provider_sdk/host_check.hpp"
#include "anolis/provider_sdk/runtime.hpp"
#include "anolis/provider_sdk/values.hpp"
#include "anolis/provider_sdk/version.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace sdk = anolis::provider_sdk;
namespace cfg = anolis::provider_sdk::config;
namespace adpp = anolis::deviceprovider::v1;

namespace {

constexpr const char* kName = "anolis-provider-reference";
constexpr double kTimeConstantS = 60.0;
constexpr uint32_t kSetPower = 1;
constexpr uint32_t kResetEnergy = 2;

// ---- config ---------------------------------------------------------------------

const cfg::Schema& config_schema() {
    static const cfg::Schema schema = [] {
        cfg::Object device(cfg::Openness::Closed);
        device.field(cfg::string_field("id").required().unique().pattern("^[a-z][a-z0-9_]*$").title("Device id"));
        device.field(cfg::string_field("label").non_empty().title("Label"));
        device.field(cfg::number_field("ambient_c").default_number(20.0).title("Ambient temperature (degC)"));
        device.field(cfg::number_field("max_rise_c")
                         .exclusive_min_number(0.0)
                         .default_number(50.0)
                         .title("Rise above ambient at full power (degC)"));
        device.field(
            cfg::number_field("rated_w").exclusive_min_number(0.0).default_number(100.0).title("Power at full (W)"));
        cfg::Object root(cfg::Openness::Closed);
        root.array("devices", cfg::Array::of_objects(std::move(device)).min_items(1), cfg::Presence::Required);
        return cfg::Schema(std::move(root)).title("anolis-provider-reference config");
    }();
    return schema;
}

struct HeaterConfig {
    std::string id;
    std::string label;
    double ambient_c = 20.0;
    double max_rise_c = 50.0;
    double rated_w = 100.0;
};

// Parses and validates the config file; throws with every error on a bad one.
std::vector<HeaterConfig> load_config(const std::string& path) {
    const YAML::Node root = YAML::LoadFile(path);
    if (const auto errors = cfg::validate(config_schema(), root); !errors.empty()) {
        throw std::runtime_error("invalid config '" + path + "':\n" + cfg::format_errors(errors));
    }
    std::vector<HeaterConfig> heaters;
    for (const auto& node : root["devices"]) {
        HeaterConfig h;
        h.id = cfg::as_string(node["id"]).value_or("");
        h.label = cfg::as_string(node["label"]).value_or(h.id);
        h.ambient_c = cfg::as_double(node["ambient_c"]).value_or(h.ambient_c);
        h.max_rise_c = cfg::as_double(node["max_rise_c"]).value_or(h.max_rise_c);
        h.rated_w = cfg::as_double(node["rated_w"]).value_or(h.rated_w);
        heaters.push_back(std::move(h));
    }
    return heaters;
}

// ---- the simulated device -----------------------------------------------------------

class Heater {
public:
    explicit Heater(const HeaterConfig& c)
        : config_(c), temperature_(c.ambient_c), at_(std::chrono::steady_clock::now()) {}

    // Moves the model to now; every read and call does this first.
    void advance() {
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - at_).count();
        at_ = now;
        const double target = config_.ambient_c + power_ * config_.max_rise_c;
        temperature_ = target + (temperature_ - target) * std::exp(-dt / kTimeConstantS);
        energy_ += power_ * config_.rated_w * dt;
    }

    const HeaterConfig& config() const { return config_; }
    double temperature() const { return temperature_; }
    double power() const { return power_; }
    double energy() const { return energy_; }
    void set_power(double power) { power_ = power; }
    double reset_energy() { return std::exchange(energy_, 0.0); }

private:
    HeaterConfig config_;
    double temperature_;
    double power_ = 0.0;
    double energy_ = 0.0;
    std::chrono::steady_clock::time_point at_;
};

adpp::SignalSpec signal(const std::string& id, const std::string& unit, const std::string& description) {
    adpp::SignalSpec s;
    s.set_signal_id(id);
    s.set_name(id);
    s.set_description(description);
    s.set_value_type(adpp::VALUE_TYPE_DOUBLE);
    s.set_unit(unit);
    s.set_poll_hint_hz(1.0);  // the runtime polls only signals with a positive hint
    return s;
}

adpp::CapabilitySet heater_capabilities() {
    adpp::CapabilitySet caps;
    *caps.add_signals() = signal("temperature", "degC", "Simulated temperature");
    *caps.add_signals() = signal("power", "", "Power level, 0 (off) to 1 (full)");
    *caps.add_signals() = signal("energy", "J", "Energy drawn since the last reset");

    // ACTUATE with a required numeric argument whose 0 is safe: the runtime's
    // zero_is_safe rung can stop it by calling it with 0. The SDK enforces the
    // declared bounds before call() runs.
    adpp::FunctionSpec* set_power = caps.add_functions();
    set_power->set_function_id(kSetPower);
    set_power->set_name("set_power");
    set_power->set_description("Set the power level; 0 turns the heater off");
    *set_power->mutable_policy() = sdk::make_function_policy(adpp::FunctionPolicy::CATEGORY_ACTUATE, false, true);
    adpp::ArgSpec* power = set_power->add_args();
    *power = sdk::make_arg_spec("power", adpp::VALUE_TYPE_DOUBLE, true, "0 (off) to 1 (full)");
    power->set_min_double(0.0);
    power->set_max_double(1.0);

    adpp::FunctionSpec* reset = caps.add_functions();
    reset->set_function_id(kResetEnergy);
    reset->set_name("reset_energy");
    reset->set_description("Set the energy count to zero");
    *reset->mutable_policy() = sdk::make_function_policy(adpp::FunctionPolicy::CATEGORY_CONFIG);
    *reset->add_results() =
        sdk::make_arg_spec("energy", adpp::VALUE_TYPE_DOUBLE, true, "The energy count before the reset", "J");
    return caps;
}

// ---- the provider -----------------------------------------------------------------

class ReferenceRuntime : public sdk::ProviderRuntime {
public:
    explicit ReferenceRuntime(const std::vector<HeaterConfig>& configs) : caps_(heater_capabilities()) {
        for (const auto& c : configs) {
            order_.push_back(c.id);
            heaters_.emplace(c.id, Heater(c));
        }
    }

    // Puts every heater in its safe state: power 0.
    void stop() {
        for (auto& [id, heater] : heaters_) {
            heater.advance();
            heater.set_power(0.0);
        }
    }

    sdk::ProviderMetadata metadata() const override {
        sdk::ProviderMetadata meta;
        meta.name = kName;
        meta.version = sdk::kVersion;
        meta.hello_extra["supports_wait_ready"] = "true";
        meta.hello_extra["conformance_level"] = "2";  // checked against config/conformance.toml
        return meta;
    }

    sdk::ReadinessReport readiness() const override {
        sdk::ReadinessReport report;
        report.configured_device_count = static_cast<int>(order_.size());
        report.successful_device_ids = order_;
        report.provider_impl = "reference";
        report.extra_diagnostics["init_time_ms"] = "0";  // nothing to initialise
        return report;
    }

    std::vector<std::string> list_device_ids() const override { return order_; }
    bool has_device(const std::string& id) const override { return heaters_.count(id) != 0; }

    adpp::Device device_info(const std::string& id) const override {
        adpp::Device d;
        d.set_device_id(id);
        d.set_provider_name(kName);
        d.set_type_id("heater");
        d.set_type_version("1");
        d.set_label(heaters_.at(id).config().label);
        return d;
    }

    adpp::CapabilitySet capabilities(const std::string&) const override { return caps_; }

    sdk::AdapterReadResult read(const std::string& id, const std::vector<std::string>& signal_ids) override {
        Heater& heater = heaters_.at(id);
        heater.advance();
        const std::map<std::string, double> values = {
            {"temperature", heater.temperature()}, {"power", heater.power()}, {"energy", heater.energy()}};
        // An empty request reads the default set; energy is read on request only.
        const std::vector<std::string> ids =
            signal_ids.empty() ? std::vector<std::string>{"temperature", "power"} : signal_ids;
        sdk::AdapterReadResult result;
        result.ok = true;
        result.error_code = adpp::Status::CODE_OK;
        for (const auto& sid : ids) {
            result.values.push_back(sdk::make_signal_value(sid, sdk::make_double_val(values.at(sid))));
        }
        return result;
    }

    sdk::AdapterCallResult call(const std::string& id, uint32_t function_id, const sdk::ValueMap& args) override {
        Heater& heater = heaters_.at(id);
        heater.advance();
        if (function_id == kSetPower) {
            double power = 0.0;
            sdk::get_arg_double(args, "power", power);  // the SDK has checked it is present and in [0, 1]
            heater.set_power(power);
            return sdk::call_ok();
        }
        if (function_id == kResetEnergy) {
            sdk::ValueMap results;
            results["energy"] = sdk::make_double_val(heater.reset_energy());
            return sdk::call_ok(std::move(results));
        }
        return sdk::call_not_found("unknown function_id " + std::to_string(function_id));
    }

    std::optional<uint32_t> resolve_function_id(const std::string&, const std::string& name) const override {
        if (name == "set_power") return kSetPower;
        if (name == "reset_energy") return kResetEnergy;
        return std::nullopt;
    }

private:
    adpp::CapabilitySet caps_;
    std::vector<std::string> order_;
    std::map<std::string, Heater> heaters_;
};

int usage(const char* program) {
    std::cerr << "usage: " << program << " --version | --config-schema | --check-config <path> | "
              << "--check-host <path> | --config <path>\n";
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() == 1 && args[0] == "--version") {
        std::cout << kName << ' ' << sdk::kVersion << '\n';
        return 0;
    }
    if (args.size() == 1 && args[0] == "--config-schema") {
        cfg::EnvelopeOptions options;
        options.provider_name = kName;
        cfg::write_config_schema_envelope(std::cout, config_schema(), options);
        return 0;
    }
    if (args.size() != 2 || (args[0] != "--check-config" && args[0] != "--check-host" && args[0] != "--config")) {
        return usage(argv[0]);
    }

    std::vector<HeaterConfig> configs;
    try {
        configs = load_config(args[1]);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return args[0] == "--check-host" ? 2 : 1;  // executable profile v1 §6: 2 = could not evaluate
    }
    if (args[0] == "--check-config") {
        std::cerr << "config valid: " << configs.size() << " heater(s)\n";
        return 0;
    }
    if (args[0] == "--check-host") {
        // A simulated device needs nothing from the host. A real provider lists
        // what it needs here (a bus device node, a group membership, ...).
        return sdk::host_check::write_envelope(std::cout, kName, {});
    }

#ifdef _WIN32
    // Frames are binary: no newline translation on the protocol stream.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    ReferenceRuntime runtime(configs);
    sdk::LifecycleHooks hooks;
    hooks.on_shutdown = [&runtime] { runtime.stop(); };
    return sdk::run_loop(std::cin, std::cout, runtime, hooks);
}
