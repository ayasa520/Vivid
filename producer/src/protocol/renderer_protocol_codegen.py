"""Generate the C contract for the private vivid-renderer-v1 protocol."""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any


WIRE_TYPE_SIZE = {
    "u8": 1,
    "u16": 2,
    "u32": 4,
    "u64": 8,
    "f32": 4,
    "f64": 8,
    "uuid": 16,
}

DIRECTION_VALUE = {
    "daemon-to-worker": "VIVID_RENDERER_DIRECTION_DAEMON_TO_WORKER",
    "worker-to-daemon": "VIVID_RENDERER_DIRECTION_WORKER_TO_DAEMON",
}

REQUEST_ID_VALUE = {
    "zero": "VIVID_RENDERER_REQUEST_ID_ZERO",
    "required": "VIVID_RENDERER_REQUEST_ID_REQUIRED",
    "any": "VIVID_RENDERER_REQUEST_ID_ANY",
}


@dataclass(frozen=True)
class WireField:
    name: str
    type: str
    offset: int
    size: int


@dataclass(frozen=True)
class WireRecord:
    slug: str
    fields: tuple[WireField, ...]
    size: int


@dataclass(frozen=True)
class WireTail:
    name: str
    type: str
    count_fields: tuple[str, ...]
    min_count: int
    max_count: int
    element_size: int
    record: str | None


@dataclass(frozen=True)
class WireMessage:
    slug: str
    name: str
    opcode: int
    direction: str
    request_id: str
    reply_to: str | None
    fields: tuple[WireField, ...]
    fixed_size: int
    tails: tuple[WireTail, ...]
    max_payload: int
    fd_rule: str
    fd_count: int
    fd_fields: tuple[str, ...]
    fd_max: int


def _token(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9]+", "_", value).strip("_").upper()


def _parse_fields(raw_fields: list[dict[str, Any]], owner: str) -> tuple[tuple[WireField, ...], int]:
    fields: list[WireField] = []
    names: set[str] = set()
    offset = 0
    for raw in raw_fields:
        name = raw["name"]
        field_type = raw["type"]
        if name in names:
            raise ValueError(f"duplicate field {name} in {owner}")
        if field_type not in WIRE_TYPE_SIZE:
            raise ValueError(f"unsupported field type {field_type} in {owner}.{name}")
        size = WIRE_TYPE_SIZE[field_type]
        fields.append(WireField(name=name, type=field_type, offset=offset, size=size))
        names.add(name)
        offset += size
    return tuple(fields), offset


def _constant(constants: dict[str, int], raw: int | str, owner: str) -> int:
    if isinstance(raw, int):
        value = raw
    else:
        if raw not in constants:
            raise ValueError(f"unknown constant {raw} in {owner}")
        value = constants[raw]
    if value < 0:
        raise ValueError(f"negative limit {value} in {owner}")
    return value


def _parse_records(spec: dict[str, Any]) -> dict[str, WireRecord]:
    records: dict[str, WireRecord] = {}
    for slug, raw in spec.get("records", {}).items():
        fields, size = _parse_fields(raw.get("fields", []), f"records.{slug}")
        if size == 0:
            raise ValueError(f"record {slug} must not be empty")
        records[slug] = WireRecord(slug=slug, fields=fields, size=size)
    return records


def _parse_messages(
    spec: dict[str, Any], constants: dict[str, int], records: dict[str, WireRecord]
) -> list[WireMessage]:
    messages: list[WireMessage] = []
    opcodes: set[int] = set()
    names: set[str] = set()

    for slug, raw in spec.get("messages", {}).items():
        name = raw["name"]
        opcode = raw["opcode"]
        direction = raw["direction"]
        request_id = raw["request-id"]
        if opcode in opcodes:
            raise ValueError(f"duplicate renderer opcode {opcode:#x}")
        if name in names:
            raise ValueError(f"duplicate renderer message name {name}")
        if direction not in DIRECTION_VALUE:
            raise ValueError(f"invalid direction {direction} for {name}")
        if request_id not in REQUEST_ID_VALUE:
            raise ValueError(f"invalid request-id policy {request_id} for {name}")

        fields, fixed_size = _parse_fields(raw.get("fields", []), f"messages.{slug}")
        field_by_name = {field.name: field for field in fields}
        tails: list[WireTail] = []
        max_payload = fixed_size
        for tail_raw in raw.get("tail", []):
            tail_type = tail_raw["type"]
            count_fields = tuple(
                [tail_raw["length-field"]]
                if "length-field" in tail_raw
                else tail_raw.get("count-fields", [])
            )
            if not count_fields:
                raise ValueError(f"tail {slug}.{tail_raw['name']} has no length/count field")
            for count_field in count_fields:
                field = field_by_name.get(count_field)
                if field is None or field.type not in {"u8", "u16", "u32", "u64"}:
                    raise ValueError(
                        f"tail {slug}.{tail_raw['name']} references invalid count field {count_field}"
                    )

            record_name = tail_raw.get("record")
            if tail_type in {"utf8", "bytes"}:
                element_size = 1
            elif tail_type.endswith("[]") and tail_type != "record[]":
                element_type = tail_type[:-2]
                if element_type not in WIRE_TYPE_SIZE or element_type == "uuid":
                    raise ValueError(f"invalid array tail type {tail_type} in {slug}")
                element_size = WIRE_TYPE_SIZE[element_type]
            elif tail_type == "record[]":
                if record_name not in records:
                    raise ValueError(f"unknown record {record_name} in {slug}")
                element_size = records[record_name].size
            else:
                raise ValueError(f"unsupported tail type {tail_type} in {slug}")

            min_count = _constant(
                constants, tail_raw.get("min", 0), f"messages.{slug}.tail"
            )
            max_count = _constant(constants, tail_raw["max"], f"messages.{slug}.tail")
            if min_count > max_count:
                raise ValueError(f"tail {slug}.{tail_raw['name']} has min greater than max")
            max_payload += max_count * element_size
            tails.append(
                WireTail(
                    name=tail_raw["name"],
                    type=tail_type,
                    count_fields=count_fields,
                    min_count=min_count,
                    max_count=max_count,
                    element_size=element_size,
                    record=record_name,
                )
            )

        fds = raw["fds"]
        fd_rule = fds["rule"]
        fd_count = 0
        fd_fields: tuple[str, ...] = ()
        if fd_rule == "exact":
            fd_count = int(fds["count"])
            fd_max = fd_count
        elif fd_rule == "product":
            fd_fields = tuple(fds["fields"])
            if not fd_fields:
                raise ValueError(f"product FD rule for {name} has no fields")
            for field_name in fd_fields:
                field = field_by_name.get(field_name)
                if field is None or field.type not in {"u8", "u16", "u32", "u64"}:
                    raise ValueError(f"invalid FD count field {field_name} for {name}")
            fd_max = _constant(constants, fds["max"], f"messages.{slug}.fds")
        else:
            raise ValueError(f"unsupported FD rule {fd_rule} for {name}")

        messages.append(
            WireMessage(
                slug=slug,
                name=name,
                opcode=opcode,
                direction=direction,
                request_id=request_id,
                reply_to=raw.get("reply-to"),
                fields=fields,
                fixed_size=fixed_size,
                tails=tuple(tails),
                max_payload=max_payload,
                fd_rule=fd_rule,
                fd_count=fd_count,
                fd_fields=fd_fields,
                fd_max=fd_max,
            )
        )
        opcodes.add(opcode)
        names.add(name)

    if not messages:
        raise ValueError("renderer protocol must declare at least one message")
    messages.sort(key=lambda message: message.opcode)

    for message in messages:
        if message.reply_to is not None and message.reply_to not in names:
            raise ValueError(f"{message.name} replies to unknown message {message.reply_to}")
    return messages


def _emit_enum(raw: dict[str, Any]) -> list[str]:
    lines = ["typedef enum", "{"]
    seen_names: set[str] = set()
    seen_values: set[int] = set()
    for member in raw["members"]:
        name = member["name"]
        value = member["value"]
        if name in seen_names or value in seen_values:
            raise ValueError(f"duplicate member in enum {raw['c-name']}: {name}={value}")
        lines.append(f"    {raw['prefix']}{name} = {value},")
        seen_names.add(name)
        seen_values.add(value)
    lines.append(f"}} {raw['c-name']};")
    return lines


def _read_expr(field: WireField, base: str = "payload") -> str:
    reader = {
        "u8": "vivid_renderer_wire_read_u8",
        "u16": "vivid_renderer_wire_read_u16",
        "u32": "vivid_renderer_wire_read_u32",
        "u64": "vivid_renderer_wire_read_u64",
    }.get(field.type)
    if reader is None:
        raise ValueError(f"field {field.name} cannot be used as a count")
    return f"{reader}({base} + {field.offset}u)"


def _emit_payload_case(message: WireMessage) -> list[str]:
    token = _token(message.name)
    if not message.tails:
        return [
            f"    case VIVID_RENDERER_MSG_{token}:",
            f"        return payload_len == VIVID_RENDERER_{token}_FIXED_BYTES;",
        ]

    fields = {field.name: field for field in message.fields}
    lines = [
        f"    case VIVID_RENDERER_MSG_{token}: {{",
        f"        if (payload_len < VIVID_RENDERER_{token}_FIXED_BYTES)",
        "            return false;",
        f"        size_t expected = VIVID_RENDERER_{token}_FIXED_BYTES;",
    ]
    for tail in message.tails:
        count_name = f"count_{_token(tail.name).lower()}"
        lines.append(f"        size_t {count_name} = 1u;")
        for count_field in tail.count_fields:
            expression = _read_expr(fields[count_field])
            component_name = f"component_{count_field}_{_token(tail.name).lower()}"
            lines.extend(
                [
                    f"        const size_t {component_name} = (size_t){expression};",
                    f"        if ({component_name} != 0u &&",
                    f"            {count_name} > {tail.max_count}u / {component_name})",
                    "            return false;",
                    f"        {count_name} *= {component_name};",
                ]
            )
        if tail.min_count == 0:
            lines.extend(
                [
                    f"        if ({count_name} > {tail.max_count}u)",
                    "            return false;",
                ]
            )
        else:
            lines.extend(
                [
                    f"        if ({count_name} < {tail.min_count}u || {count_name} > {tail.max_count}u)",
                    "            return false;",
                ]
            )
        lines.extend(
            [
                f"        if ({count_name} > (SIZE_MAX - expected) / {tail.element_size}u)",
                "            return false;",
                f"        expected += {count_name} * {tail.element_size}u;",
            ]
        )
    lines.extend(["        return expected == payload_len;", "    }"])
    return lines


def _emit_fd_case(message: WireMessage) -> list[str]:
    token = _token(message.name)
    if message.fd_rule == "exact":
        return [
            f"    case VIVID_RENDERER_MSG_{token}:",
            f"        return n_fds == {message.fd_count}u;",
        ]

    fields = {field.name: field for field in message.fields}
    lines = [
        f"    case VIVID_RENDERER_MSG_{token}: {{",
        "        size_t expected = 1u;",
    ]
    for field_name in message.fd_fields:
        expression = _read_expr(fields[field_name])
        lines.extend(
            [
                f"        const size_t {field_name} = (size_t){expression};",
                f"        if ({field_name} != 0u && expected > {message.fd_max}u / {field_name})",
                "            return false;",
                f"        expected *= {field_name};",
            ]
        )
    lines.extend(
        [
            f"        return expected <= {message.fd_max}u && n_fds == expected;",
            "    }",
        ]
    )
    return lines


def emit_renderer_c(spec: dict[str, Any]) -> str:
    protocol = spec["protocol"]
    constants = {name: int(value) for name, value in spec["constants"].items()}
    records = _parse_records(spec)
    messages = _parse_messages(spec, constants, records)
    header_fields, header_size = _parse_fields(spec["header"]["fields"], "header")
    header_names = [field.name for field in header_fields]
    required_header = [
        "magic",
        "version",
        "opcode",
        "payload_length",
        "request_id",
        "renderer_instance_id",
    ]
    if header_names != required_header:
        raise ValueError(f"renderer header must be exactly {required_header}")
    if constants["MAX_PACKET_BYTES"] != header_size + constants["MAX_PAYLOAD_BYTES"]:
        raise ValueError("MAX_PACKET_BYTES must equal header bytes plus MAX_PAYLOAD_BYTES")
    if any(message.max_payload > constants["MAX_PAYLOAD_BYTES"] for message in messages):
        raise ValueError("a renderer message exceeds MAX_PAYLOAD_BYTES")

    banner = "/* generated by protocol_gen.py from vivid_renderer_v1.toml — do not edit */"
    lines = [
        banner,
        "",
        "#ifndef VIVID_RENDERER_PROTOCOL_H",
        "#define VIVID_RENDERER_PROTOCOL_H",
        "",
        "#include <stdbool.h>",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "#include <string.h>",
        "",
        f'#define VIVID_RENDERER_PROTOCOL_NAME "{protocol["name"]}"',
        f"#define VIVID_RENDERER_PROTOCOL_MAGIC UINT32_C(0x{protocol['magic']:08x})",
        f"#define VIVID_RENDERER_PROTOCOL_VERSION {protocol['version']}u",
        f"#define VIVID_RENDERER_SPAWN_VERSION {protocol['spawn-version']}u",
        f"#define VIVID_RENDERER_HEADER_BYTES {header_size}u",
    ]
    for name, value in constants.items():
        lines.append(f"#define VIVID_RENDERER_{name} {value}u")
    lines.append("")

    for field in header_fields:
        lines.append(f"#define VIVID_RENDERER_HEADER_{_token(field.name)}_OFFSET {field.offset}u")
    lines.append("")

    for enum_spec in spec.get("enums", {}).values():
        lines.extend(_emit_enum(enum_spec))
        lines.append("")

    lines.extend(
        [
            "typedef enum",
            "{",
            "    VIVID_RENDERER_DIRECTION_DAEMON_TO_WORKER = 1,",
            "    VIVID_RENDERER_DIRECTION_WORKER_TO_DAEMON = 2,",
            "} VividRendererMessageDirection;",
            "",
            "typedef enum",
            "{",
            "    VIVID_RENDERER_REQUEST_ID_ZERO = 1,",
            "    VIVID_RENDERER_REQUEST_ID_REQUIRED = 2,",
            "    VIVID_RENDERER_REQUEST_ID_ANY = 3,",
            "} VividRendererRequestIdPolicy;",
            "",
            "typedef enum",
            "{",
        ]
    )
    for message in messages:
        lines.append(f"    VIVID_RENDERER_MSG_{_token(message.name)} = 0x{message.opcode:04x},")
    lines.extend(["} VividRendererOpcode;", ""])

    for record in records.values():
        record_token = _token(record.slug)
        lines.append(f"#define VIVID_RENDERER_{record_token}_BYTES {record.size}u")
        for field in record.fields:
            lines.append(
                f"#define VIVID_RENDERER_{record_token}_{_token(field.name)}_OFFSET {field.offset}u"
            )
        lines.append("")

    for message in messages:
        token = _token(message.name)
        lines.append(f"#define VIVID_RENDERER_{token}_FIXED_BYTES {message.fixed_size}u")
        lines.append(f"#define VIVID_RENDERER_{token}_MAX_PAYLOAD_BYTES {message.max_payload}u")
        lines.append(
            f"#define VIVID_RENDERER_{token}_FD_COUNT_MIN "
            f"{message.fd_count if message.fd_rule == 'exact' else 0}u"
        )
        lines.append(f"#define VIVID_RENDERER_{token}_FD_COUNT_MAX {message.fd_max}u")
        for field in message.fields:
            lines.append(
                f"#define VIVID_RENDERER_{token}_{_token(field.name)}_OFFSET {field.offset}u"
            )
        lines.append("")

    lines.extend(
        [
            "typedef struct",
            "{",
            "    uint32_t magic;",
            "    uint16_t version;",
            "    uint16_t opcode;",
            "    uint32_t payload_length;",
            "    uint64_t request_id;",
            "    uint64_t renderer_instance_id;",
            "} VividRendererMessageHeader;",
            "",
            "typedef struct",
            "{",
            "    uint16_t opcode;",
            "    VividRendererMessageDirection direction;",
            "    VividRendererRequestIdPolicy request_id_policy;",
            "    uint32_t min_payload_bytes;",
            "    uint32_t max_payload_bytes;",
            "    uint32_t min_fd_count;",
            "    uint32_t max_fd_count;",
            "    const char* name;",
            "    const char* reply_to;",
            "} VividRendererMessageDescriptor;",
            "",
            "static inline uint8_t vivid_renderer_wire_read_u8(const uint8_t* in)",
            "{",
            "    return in[0];",
            "}",
            "",
            "static inline uint16_t vivid_renderer_wire_read_u16(const uint8_t* in)",
            "{",
            "    return (uint16_t)in[0] | ((uint16_t)in[1] << 8);",
            "}",
            "",
            "static inline uint32_t vivid_renderer_wire_read_u32(const uint8_t* in)",
            "{",
            "    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) |",
            "           ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);",
            "}",
            "",
            "static inline uint64_t vivid_renderer_wire_read_u64(const uint8_t* in)",
            "{",
            "    return (uint64_t)vivid_renderer_wire_read_u32(in) |",
            "           ((uint64_t)vivid_renderer_wire_read_u32(in + 4) << 32);",
            "}",
            "",
            "static inline float vivid_renderer_wire_read_f32(const uint8_t* in)",
            "{",
            "    const uint32_t bits = vivid_renderer_wire_read_u32(in);",
            "    float value = 0.0f;",
            "    memcpy(&value, &bits, sizeof(value));",
            "    return value;",
            "}",
            "",
            "static inline double vivid_renderer_wire_read_f64(const uint8_t* in)",
            "{",
            "    const uint64_t bits = vivid_renderer_wire_read_u64(in);",
            "    double value = 0.0;",
            "    memcpy(&value, &bits, sizeof(value));",
            "    return value;",
            "}",
            "",
            "static inline void vivid_renderer_wire_write_u16(uint8_t* out, uint16_t value)",
            "{",
            "    out[0] = (uint8_t)(value & 0xffu);",
            "    out[1] = (uint8_t)((value >> 8) & 0xffu);",
            "}",
            "",
            "static inline void vivid_renderer_wire_write_u32(uint8_t* out, uint32_t value)",
            "{",
            "    out[0] = (uint8_t)(value & 0xffu);",
            "    out[1] = (uint8_t)((value >> 8) & 0xffu);",
            "    out[2] = (uint8_t)((value >> 16) & 0xffu);",
            "    out[3] = (uint8_t)((value >> 24) & 0xffu);",
            "}",
            "",
            "static inline void vivid_renderer_wire_write_u64(uint8_t* out, uint64_t value)",
            "{",
            "    vivid_renderer_wire_write_u32(out, (uint32_t)value);",
            "    vivid_renderer_wire_write_u32(out + 4, (uint32_t)(value >> 32));",
            "}",
            "",
            "static inline void vivid_renderer_wire_write_f32(uint8_t* out, float value)",
            "{",
            "    uint32_t bits = 0;",
            "    memcpy(&bits, &value, sizeof(bits));",
            "    vivid_renderer_wire_write_u32(out, bits);",
            "}",
            "",
            "static inline void vivid_renderer_wire_write_f64(uint8_t* out, double value)",
            "{",
            "    uint64_t bits = 0;",
            "    memcpy(&bits, &value, sizeof(bits));",
            "    vivid_renderer_wire_write_u64(out, bits);",
            "}",
            "",
            "static inline bool vivid_renderer_header_encode(",
            "    uint8_t out[VIVID_RENDERER_HEADER_BYTES],",
            "    const VividRendererMessageHeader* header)",
            "{",
            "    if (!out || !header)",
            "        return false;",
            "    vivid_renderer_wire_write_u32(out + VIVID_RENDERER_HEADER_MAGIC_OFFSET, header->magic);",
            "    vivid_renderer_wire_write_u16(out + VIVID_RENDERER_HEADER_VERSION_OFFSET, header->version);",
            "    vivid_renderer_wire_write_u16(out + VIVID_RENDERER_HEADER_OPCODE_OFFSET, header->opcode);",
            "    vivid_renderer_wire_write_u32(out + VIVID_RENDERER_HEADER_PAYLOAD_LENGTH_OFFSET, header->payload_length);",
            "    vivid_renderer_wire_write_u64(out + VIVID_RENDERER_HEADER_REQUEST_ID_OFFSET, header->request_id);",
            "    vivid_renderer_wire_write_u64(out + VIVID_RENDERER_HEADER_RENDERER_INSTANCE_ID_OFFSET, header->renderer_instance_id);",
            "    return true;",
            "}",
            "",
            "static inline bool vivid_renderer_header_decode(",
            "    const uint8_t* data, size_t len, VividRendererMessageHeader* header)",
            "{",
            "    if (!data || !header || len != VIVID_RENDERER_HEADER_BYTES)",
            "        return false;",
            "    header->magic = vivid_renderer_wire_read_u32(data + VIVID_RENDERER_HEADER_MAGIC_OFFSET);",
            "    header->version = vivid_renderer_wire_read_u16(data + VIVID_RENDERER_HEADER_VERSION_OFFSET);",
            "    header->opcode = vivid_renderer_wire_read_u16(data + VIVID_RENDERER_HEADER_OPCODE_OFFSET);",
            "    header->payload_length = vivid_renderer_wire_read_u32(data + VIVID_RENDERER_HEADER_PAYLOAD_LENGTH_OFFSET);",
            "    header->request_id = vivid_renderer_wire_read_u64(data + VIVID_RENDERER_HEADER_REQUEST_ID_OFFSET);",
            "    header->renderer_instance_id = vivid_renderer_wire_read_u64(data + VIVID_RENDERER_HEADER_RENDERER_INSTANCE_ID_OFFSET);",
            "    return true;",
            "}",
            "",
            "static inline const VividRendererMessageDescriptor*",
            "vivid_renderer_message_descriptor(uint16_t opcode)",
            "{",
            "    switch (opcode) {",
        ]
    )
    for message in messages:
        token = _token(message.name)
        reply_to = f'"{message.reply_to}"' if message.reply_to is not None else "NULL"
        lines.extend(
            [
                f"    case VIVID_RENDERER_MSG_{token}: {{",
                "        static const VividRendererMessageDescriptor descriptor = {",
                f"            VIVID_RENDERER_MSG_{token},",
                f"            {DIRECTION_VALUE[message.direction]},",
                f"            {REQUEST_ID_VALUE[message.request_id]},",
                f"            VIVID_RENDERER_{token}_FIXED_BYTES,",
                f"            VIVID_RENDERER_{token}_MAX_PAYLOAD_BYTES,",
                f"            VIVID_RENDERER_{token}_FD_COUNT_MIN,",
                f"            VIVID_RENDERER_{token}_FD_COUNT_MAX,",
                f'            "{message.name}",',
                f"            {reply_to},",
                "        };",
                "        return &descriptor;",
                "    }",
            ]
        )
    lines.extend(["    default:", "        return NULL;", "    }", "}", ""])

    lines.extend(
        [
            "/*",
            " * Variable payload lengths are derived only from generated offsets and",
            " * limits. The transport calls this before any backend sees a packet, so",
            " * malformed lengths cannot make renderer-specific parsers walk past the",
            " * received SOCK_SEQPACKET record.",
            " */",
            "static inline bool vivid_renderer_payload_length_valid(",
            "    uint16_t opcode, const uint8_t* payload, size_t payload_len)",
            "{",
            "    if ((!payload && payload_len != 0u) || payload_len > VIVID_RENDERER_MAX_PAYLOAD_BYTES)",
            "        return false;",
            "    switch (opcode) {",
        ]
    )
    for message in messages:
        lines.extend(_emit_payload_case(message))
    lines.extend(["    default:", "        return false;", "    }", "}", ""])

    lines.extend(
        [
            "/*",
            " * SCM_RIGHTS cardinality is part of the wire contract. BIND_BUFFERS is",
            " * the only dynamic case: one moved descriptor is required for every",
            " * serialized buffer/plane tuple. This check runs while all received FDs",
            " * still have a single temporary owner, allowing the caller to close the",
            " * whole set atomically on any mismatch.",
            " */",
            "static inline bool vivid_renderer_fd_count_valid(",
            "    uint16_t opcode, const uint8_t* payload, size_t payload_len, size_t n_fds)",
            "{",
            "    if (!vivid_renderer_payload_length_valid(opcode, payload, payload_len) ||",
            "        n_fds > VIVID_RENDERER_MAX_FDS_PER_MESSAGE)",
            "        return false;",
            "    switch (opcode) {",
        ]
    )
    for message in messages:
        lines.extend(_emit_fd_case(message))
    lines.extend(["    default:", "        return false;", "    }", "}", ""])

    lines.extend(
        [
            "static inline bool vivid_renderer_header_contract_valid(",
            "    const VividRendererMessageHeader* header,",
            "    uint64_t expected_instance_id,",
            "    VividRendererMessageDirection expected_direction)",
            "{",
            "    if (!header || header->magic != VIVID_RENDERER_PROTOCOL_MAGIC ||",
            "        header->version != VIVID_RENDERER_PROTOCOL_VERSION ||",
            "        header->renderer_instance_id == 0u ||",
            "        header->renderer_instance_id != expected_instance_id ||",
            "        header->payload_length > VIVID_RENDERER_MAX_PAYLOAD_BYTES)",
            "        return false;",
            "    const VividRendererMessageDescriptor* descriptor =",
            "        vivid_renderer_message_descriptor(header->opcode);",
            "    if (!descriptor || descriptor->direction != expected_direction)",
            "        return false;",
            "    if (descriptor->request_id_policy == VIVID_RENDERER_REQUEST_ID_ZERO)",
            "        return header->request_id == 0u;",
            "    if (descriptor->request_id_policy == VIVID_RENDERER_REQUEST_ID_REQUIRED)",
            "        return header->request_id != 0u;",
            "    return true;",
            "}",
            "",
            "#endif",
            "",
        ]
    )
    return "\n".join(lines)


def emit_renderer_docs(spec: dict[str, Any]) -> str:
    protocol = spec["protocol"]
    constants = {name: int(value) for name, value in spec["constants"].items()}
    records = _parse_records(spec)
    messages = _parse_messages(spec, constants, records)
    _header_fields, header_size = _parse_fields(spec["header"]["fields"], "header")
    lines = [
        "<!-- generated by protocol_gen.py from vivid_renderer_v1.toml — do not edit -->",
        "",
        "# vivid-renderer-v1 generated reference",
        "",
        f"Protocol version: `{protocol['version']}`. Spawn version: `{protocol['spawn-version']}`.",
        f"Header size: `{header_size}` bytes. Byte order: `{protocol['byte-order']}`.",
        "",
        "| Opcode | Direction | Message | Request ID | Fixed bytes | Maximum bytes | FD rule | Reply to |",
        "|---:|---|---|---|---:|---:|---|---|",
    ]
    for message in messages:
        if message.fd_rule == "exact":
            fd_rule = f"exactly {message.fd_count}"
        else:
            fd_rule = " × ".join(f"`{field}`" for field in message.fd_fields)
        lines.append(
            f"| `0x{message.opcode:04x}` | {message.direction} | `{message.name}` | "
            f"{message.request_id} | {message.fixed_size} | {message.max_payload} | "
            f"{fd_rule} | {message.reply_to or ''} |"
        )
    lines.extend(["", "## Fixed field layouts", ""])
    for message in messages:
        lines.extend([f"### {message.name}", ""])
        if not message.fields:
            lines.extend(["No payload fields.", ""])
            continue
        lines.extend(["| Field | Type | Offset | Bytes |", "|---|---|---:|---:|"])
        for field in message.fields:
            lines.append(f"| `{field.name}` | `{field.type}` | {field.offset} | {field.size} |")
        for tail in message.tails:
            count = " × ".join(f"`{field}`" for field in tail.count_fields)
            lines.append(
                f"| `{tail.name}` | `{tail.type}` tail ({count}, {tail.min_count}..{tail.max_count}) | "
                f"{message.fixed_size} + previous tails | {tail.element_size} each |"
            )
        lines.append("")
    return "\n".join(lines)


def build_renderer_outputs(spec: dict[str, Any], c_out: Path, docs_out: Path) -> dict[Path, str]:
    return {
        c_out: emit_renderer_c(spec),
        docs_out: emit_renderer_docs(spec),
    }
