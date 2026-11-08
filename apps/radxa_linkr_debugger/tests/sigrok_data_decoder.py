"""Pure v2 DATA decoder used by the live WebSocket test and offline tests.

No transport, environment checks, optional dependencies, or device access belongs
in this module.
"""

COMPRESSION_NONE = 0
COMPRESSION_BIT_PACK = 1
COMPRESSION_RLE = 2
COMPRESSION_BIT_PACK_RLE = 3
COMPRESSION_SINGLE_BITS = 4
COMPRESSION_SINGLE_BITS_RLE = 5
COMPRESSION_PACKED_PALETTE2 = 6
COMPRESSION_DENSE_BITS = 7
SIGROK_MAX_SAMPLE_PAYLOAD_BYTES = 4096


def sigrok_bytes_per_sample(channel_mask: int) -> int:
    channel_count = (channel_mask & 0xFFFF).bit_count()
    if channel_count == 0:
        return 0
    return (channel_count + 7) // 8


def decode_rle_units(
    sample_payload: bytes,
    unit_count: int,
    unit_bytes: int,
    compression_name: str,
) -> bytes:
    tuple_len = unit_bytes + 2
    if len(sample_payload) == 0 or len(sample_payload) % tuple_len != 0:
        raise ValueError(f"{compression_name} payload has truncated tuple")
    out = bytearray()
    expanded_count = 0
    for pos in range(0, len(sample_payload), tuple_len):
        value = sample_payload[pos:pos + unit_bytes]
        run_pos = pos + unit_bytes
        run_count = sample_payload[run_pos] | (sample_payload[run_pos + 1] << 8)
        if run_count == 0:
            raise ValueError(f"{compression_name} run_count must be non-zero")
        if expanded_count + run_count > unit_count:
            raise ValueError(f"{compression_name} expanded unit count overflows metadata")
        out.extend(value * run_count)
        expanded_count += run_count
    if expanded_count != unit_count or len(out) != unit_count * unit_bytes:
        raise ValueError(f"{compression_name} expanded output does not match metadata")
    return bytes(out)


def decode_packed_palette2(sample_payload: bytes, sample_count: int, bytes_per_sample: int) -> bytes:
    selector_len = (sample_count + 7) // 8
    palette_bytes = bytes_per_sample * 2
    if len(sample_payload) != palette_bytes + selector_len:
        raise ValueError("PACKED_PALETTE2 payload length does not match metadata")
    palette0 = sample_payload[:bytes_per_sample]
    palette1 = sample_payload[bytes_per_sample:palette_bytes]
    if palette0 == palette1:
        raise ValueError("PACKED_PALETTE2 palette values must be distinct")
    selectors = sample_payload[palette_bytes:]
    trailing_bits = sample_count % 8
    if trailing_bits and selectors[-1] & ~((1 << trailing_bits) - 1):
        raise ValueError("PACKED_PALETTE2 selector tail padding is non-zero")
    out = bytearray()
    for sample_index in range(sample_count):
        selector = selectors[sample_index >> 3]
        out.extend(palette1 if selector & (1 << (sample_index & 7)) else palette0)
    return bytes(out)


def decode_single_bits(sample_payload: bytes, sample_count: int, channel_mask: int) -> bytes:
    if int(channel_mask & 0xFFFF).bit_count() != 1:
        raise ValueError("SINGLE_BITS requires exactly one active channel")
    expected_len = (sample_count + 7) // 8
    if len(sample_payload) != expected_len:
        raise ValueError(f"SINGLE_BITS payload length {len(sample_payload)} != expected {expected_len}")
    trailing_bits = sample_count % 8
    if trailing_bits and sample_payload[-1] & ~((1 << trailing_bits) - 1):
        raise ValueError("SINGLE_BITS payload has non-zero tail padding")
    return bytes(
        (sample_payload[sample_index >> 3] >> (sample_index & 7)) & 1
        for sample_index in range(sample_count)
    )


def decode_dense_bits(sample_payload: bytes, sample_count: int, channel_count: int) -> bytes:
    bit_count = sample_count * channel_count
    expected_len = (bit_count + 7) // 8
    if len(sample_payload) != expected_len:
        raise ValueError(f"DENSE_BITS payload length {len(sample_payload)} != expected {expected_len}")
    trailing_bits = bit_count % 8
    if trailing_bits and sample_payload[-1] & ~((1 << trailing_bits) - 1):
        raise ValueError("DENSE_BITS payload has non-zero tail padding")

    bytes_per_sample = (channel_count + 7) // 8
    out = bytearray(sample_count * bytes_per_sample)
    accumulator = 0
    available_bits = 0
    input_pos = 0
    value_mask = (1 << channel_count) - 1
    for sample_index in range(sample_count):
        while available_bits < channel_count:
            accumulator |= sample_payload[input_pos] << available_bits
            available_bits += 8
            input_pos += 1
        value = accumulator & value_mask
        accumulator >>= channel_count
        available_bits -= channel_count
        output_pos = sample_index * bytes_per_sample
        out[output_pos] = value & 0xFF
        if bytes_per_sample == 2:
            out[output_pos + 1] = value >> 8
    return bytes(out)


def decode_data_payload(meta: dict[str, int], sample_payload: bytes) -> bytes:
    sample_count = int(meta["sample_count"])
    if not 1 <= sample_count <= 0xFFFF:
        raise ValueError("DATA sample_count must be a non-zero uint16")
    channel_mask = int(meta["channel_mask"])
    if channel_mask <= 0 or channel_mask & ~0x07FF:
        raise ValueError("DATA channel_mask must be non-zero within 0x07ff")
    if len(sample_payload) > SIGROK_MAX_SAMPLE_PAYLOAD_BYTES:
        raise ValueError("DATA sample payload exceeds 4096-byte limit")
    bytes_per_sample = sigrok_bytes_per_sample(channel_mask)
    expected_len = sample_count * bytes_per_sample
    compression = int(meta["compression"])
    if compression == COMPRESSION_DENSE_BITS:
        return decode_dense_bits(sample_payload, sample_count, channel_mask.bit_count())
    if compression in (COMPRESSION_NONE, COMPRESSION_BIT_PACK):
        if len(sample_payload) != expected_len:
            raise ValueError(f"BIT_PACK payload length {len(sample_payload)} != expected {expected_len}")
        return sample_payload
    if compression == COMPRESSION_BIT_PACK_RLE:
        return decode_rle_units(sample_payload, sample_count, bytes_per_sample, "BIT_PACK_RLE")
    if compression == COMPRESSION_SINGLE_BITS:
        return decode_single_bits(sample_payload, sample_count, channel_mask)
    if compression == COMPRESSION_SINGLE_BITS_RLE:
        dense_byte_count = (sample_count + 7) // 8
        dense = decode_rle_units(sample_payload, dense_byte_count, 1, "SINGLE_BITS_RLE")
        return decode_single_bits(dense, sample_count, channel_mask)
    if compression == COMPRESSION_PACKED_PALETTE2:
        return decode_packed_palette2(sample_payload, sample_count, bytes_per_sample)
    if compression == COMPRESSION_RLE:
        raise ValueError("standalone RLE is not valid for current DATA samples")
    raise ValueError(f"unsupported DATA compression {compression}")
