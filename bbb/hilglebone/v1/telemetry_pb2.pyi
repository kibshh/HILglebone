from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from typing import ClassVar as _ClassVar, Mapping as _Mapping, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class AckResponse(_message.Message):
    __slots__ = ("sequence_num", "cmd_type", "error_code", "sensor_id")
    SEQUENCE_NUM_FIELD_NUMBER: _ClassVar[int]
    CMD_TYPE_FIELD_NUMBER: _ClassVar[int]
    ERROR_CODE_FIELD_NUMBER: _ClassVar[int]
    SENSOR_ID_FIELD_NUMBER: _ClassVar[int]
    sequence_num: int
    cmd_type: int
    error_code: int
    sensor_id: int
    def __init__(self, sequence_num: _Optional[int] = ..., cmd_type: _Optional[int] = ..., error_code: _Optional[int] = ..., sensor_id: _Optional[int] = ...) -> None: ...

class ErrorResponse(_message.Message):
    __slots__ = ("sensor_id", "error_code", "detail")
    SENSOR_ID_FIELD_NUMBER: _ClassVar[int]
    ERROR_CODE_FIELD_NUMBER: _ClassVar[int]
    DETAIL_FIELD_NUMBER: _ClassVar[int]
    sensor_id: int
    error_code: int
    detail: str
    def __init__(self, sensor_id: _Optional[int] = ..., error_code: _Optional[int] = ..., detail: _Optional[str] = ...) -> None: ...

class StatusReport(_message.Message):
    __slots__ = ("uptime_s", "active_sensor_count", "command_queue_depth")
    UPTIME_S_FIELD_NUMBER: _ClassVar[int]
    ACTIVE_SENSOR_COUNT_FIELD_NUMBER: _ClassVar[int]
    COMMAND_QUEUE_DEPTH_FIELD_NUMBER: _ClassVar[int]
    uptime_s: int
    active_sensor_count: int
    command_queue_depth: int
    def __init__(self, uptime_s: _Optional[int] = ..., active_sensor_count: _Optional[int] = ..., command_queue_depth: _Optional[int] = ...) -> None: ...

class TelemetryEnvelope(_message.Message):
    __slots__ = ("device_id", "session_id", "timestamp_us", "ack", "error", "status_report")
    DEVICE_ID_FIELD_NUMBER: _ClassVar[int]
    SESSION_ID_FIELD_NUMBER: _ClassVar[int]
    TIMESTAMP_US_FIELD_NUMBER: _ClassVar[int]
    ACK_FIELD_NUMBER: _ClassVar[int]
    ERROR_FIELD_NUMBER: _ClassVar[int]
    STATUS_REPORT_FIELD_NUMBER: _ClassVar[int]
    device_id: str
    session_id: str
    timestamp_us: int
    ack: AckResponse
    error: ErrorResponse
    status_report: StatusReport
    def __init__(self, device_id: _Optional[str] = ..., session_id: _Optional[str] = ..., timestamp_us: _Optional[int] = ..., ack: _Optional[_Union[AckResponse, _Mapping]] = ..., error: _Optional[_Union[ErrorResponse, _Mapping]] = ..., status_report: _Optional[_Union[StatusReport, _Mapping]] = ...) -> None: ...
