# Config-schema toolkit (declare-once)

The `anolis::provider_sdk_config` module lets a provider declare its config
contract **once** as a `config::Schema`; the SDK derives every downstream
surface from that single declaration, so they cannot drift:

| Surface | API | Consumer |
| --- | --- | --- |
| `--config-schema` envelope (executable profile v1 §2) | `write_config_schema_envelope()` | workbench / any client authoring config |
| `--check-config` validation | `validate()` + `format_errors()` | operators, install pipelines |
| post-validation config parsing | `as_string/as_int64/as_double/as_bool/parse_i2c_address` | the provider's own `load_config` |

Headers: `config.hpp` (model + emitter, no YAML dependency) and
`config_validate.hpp` (validator + typed extraction, yaml-cpp on the API).

## The emitted JSON Schema

Draft 2020-12; deterministic declaration-order keys. Standard keywords carry
everything they can (`type`, `enum`/`oneOf`+`title`, `const`, `not`,
`pattern`, `minLength`, `minimum`/`maximum`/`exclusiveMinimum`/
`exclusiveMaximum`, `minItems`/`maxItems`, `required`,
`additionalProperties`, `dependentRequired`, `allOf`/`if`/`then`, `default`,
`title`, `description`).

### `x-anolis-*` vocabulary

Where JSON Schema has no native slot for a constraint the SDK enforces, the
schema carries an annotation so consumers (e.g. a form renderer) keep the
information:

| Key | Where | Meaning |
| --- | --- | --- |
| `x-anolis-unique: true` | a field inside array items, or a scalar array | The **parsed** value must be unique across the array's items. Native `uniqueItems` (also emitted on scalar arrays, best-effort) compares JSON values and cannot see that `0x08` and `"0x08"` collide; the SDK validator can. |
| `x-anolis-placeholder` | any field | Form placeholder text (e.g. `/dev/i2c-1 or mock://name`). |
| `x-anolis-type: "i2c_address"` | the built-in I2C scalar | The field is `anyOf` an integer `0x08..0x77` or a `0x`-prefixed hex string covering exactly that range. Quoted **decimal** strings (`"97"`) are not addresses. |

Display titles for enum options are standard JSON Schema: a titled enum emits
`oneOf: [{const, title}, ...]`; untitled emits plain `enum`.

## Scalar typing: the YAML 1.2 core schema

`validate()` types scalars by the YAML 1.2 **core schema**, so the emitted
JSON Schema is honest about what `--check-config` accepts:

- a **quoted** scalar (or `!!str`) is a string — `timeout_ms: "300"` is a
  type error against `type: integer`;
- a **plain** scalar resolves by the core rules: `true`/`false` (only — no
  YAML-1.1 `yes`/`on`) are booleans; `300`, `0x63`, `0o17` are integers
  (`010` is decimal ten); `0.5`, `5e-3`, `.inf`, `.nan` are floats;
  anything else is a string;
- per JSON Schema semantics `type: integer` also accepts a zero-fraction
  float (`5.0`) but never `5.7`; `type: number` rejects the non-finite
  floats JSON cannot represent (`.inf`/`.nan`).

Known deviations from a pure JSON Schema validator (deliberate):

- integers outside int64 (`2^63` and beyond) are rejected by `--check-config`
  even though the emitted `type: integer` has no such cap — no provider config
  value can exceed int64;
- a **duplicated map key** is rejected outright: YAML parsers disagree
  (yaml-cpp serves the first occurrence, PyYAML the last, js-yaml throws), so
  validating either occurrence would bless an ambiguous document;
- YAML **merge keys** (`<<`) are not supported by yaml-cpp: on a Closed object
  they fail as `unknown key '<<'` (plain anchors/aliases DO work).

The typed extraction helpers use the **same resolver**, so what validation
accepted, extraction converts identically — do not mix them with yaml-cpp's
`as<T>()` (whose coercions accept what the validator rejects).

### Documented tightenings vs. the historical hand-written parsers

Adopting the validator is stricter than the providers' pre-toolkit parsing in
these corners (all shipped fleet configs verified unaffected — plain scalars
throughout):

1. **Quoted numerics rejected**: `timeout_ms: "300"` was accepted by
   stoi-based parsing; it is a string now.
2. **Trailing junk rejected**: `timeout_ms: 5abc` silently parsed as 5
   (`std::stoi` stops at the first non-digit); it is a string now.
3. **Quoted decimal addresses rejected**: `address: "97"` was accepted by
   base-10 string parsing; the string branch is strictly `0xNN` now.
4. **bread `mode: scan` with an empty `addresses: []`** was quietly accepted;
   forbid-on-scan rejects any `addresses` key now.

## Patterns

Patterns are **anchored** ECMAScript regexes (`^...$`). JSON Schema `pattern`
uses *search* semantics, and the validator matches with `std::regex_search`
to agree with external validators — an unanchored pattern would silently
accept substrings. Stay within the std::regex ECMAScript subset (no
lookbehind).

## Boundary: what stays provider-side

Conditionals (`when()`) see **sibling keys only**. A cross-scope rule — e.g.
sim's "`devices[].physics_bindings` is only valid when the root
`simulation.mode` is `sim`" — is not declarable; it remains a provider-side
semantic check after `validate()` passes, and the emitted schema is honestly
*looser* there. The same applies to dynamic defaults ("`label` defaults to
the `id`") — declare those in `description` text.

`deprecated_key()` is validator-side only: on a **Closed** object external
validators still reject the key generically (`additionalProperties: false`);
on an **Open** object the emitted schema accepts what `--check-config`
rejects, so prefer Closed objects for sections with removed keys.

## Provider rollout sketch

```cpp
// schema.cpp — the single declaration
const config::Schema& provider_schema();

// main.cpp — handle the verb BEFORE anything can touch stdout
if (arg == "--config-schema") {
    config::EnvelopeOptions options;
    options.provider_name = "anolis-provider-ezo";
    config::write_config_schema_envelope(std::cout, provider_schema(), options);
    return 0;
}

// load_config: LoadFile -> validate (throw ALL errors) -> extract via the
// typed helpers (delete the hand-written parse_int_value/parse_address_text)
const auto errors = config::validate(provider_schema(), root);
if (!errors.empty()) {
    throw std::runtime_error("invalid config:\n" + config::format_errors(errors));
}
```
