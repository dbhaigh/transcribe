"""Tiny constant-output ONNX graphs for adapter validation; no ONNX/Python dependency."""

import struct


def _varint(value):
    result = bytearray()
    while value > 127:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def _integer(field, value):
    return _varint(field << 3) + _varint(value)


def _message(field, value):
    if isinstance(value, str):
        value = value.encode()
    return _varint((field << 3) | 2) + _varint(len(value)) + value


def _value(name, dimensions, element_type=1):
    shape = b"".join(_message(1, _message(2, "samples") if dim is None else _integer(1, dim))
                     for dim in dimensions)
    tensor_type = _integer(1, element_type) + _message(2, shape)
    return _message(1, name) + _message(2, _message(1, tensor_type))


def write_model(path, *, inputs=(1, None), classes=69, output_type=1,
                input_type=1, nonfinite=False, output_name="pitch"):
    """Encode ModelProto/GraphProto/TensorProto using their standard protobuf field IDs."""
    frames = 20
    values = [0.0] * (frames * classes)
    for frame in range(frames):
        values[frame * classes + min(41, classes - 1)] = float("nan") if nonfinite else .9
    tensor = b"".join(_integer(1, dim) for dim in (1, frames, classes))
    tensor += _integer(2, output_type) + _message(8, output_name)
    tensor += _message(9, struct.pack("<" + ("d" if output_type == 11 else "f") * len(values), *values))
    graph = _message(2, "adapter-validation")
    graph += _message(5, tensor)
    graph += _message(11, _value("waveform", inputs, input_type))
    graph += _message(12, _value(output_name, (1, frames, classes), output_type))
    model = _integer(1, 8) + _message(7, graph) + _message(8, _integer(2, 13))
    path.write_bytes(model)
