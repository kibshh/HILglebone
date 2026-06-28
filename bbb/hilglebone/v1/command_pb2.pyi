from hilglebone.v1 import common_pb2 as _common_pb2
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from typing import ClassVar as _ClassVar, Mapping as _Mapping, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class SetupSensorCommand(_message.Message):
    __slots__ = ("protocol_id", "config")
    PROTOCOL_ID_FIELD_NUMBER: _ClassVar[int]
    CONFIG_FIELD_NUMBER: _ClassVar[int]
    protocol_id: _common_pb2.ProtocolId
    config: bytes
    def __init__(self, protocol_id: _Optional[_Union[_common_pb2.ProtocolId, str]] = ..., config: _Optional[bytes] = ...) -> None: ...

class SetOutputCommand(_message.Message):
    __slots__ = ("sensor_id", "values")
    SENSOR_ID_FIELD_NUMBER: _ClassVar[int]
    VALUES_FIELD_NUMBER: _ClassVar[int]
    sensor_id: int
    values: bytes
    def __init__(self, sensor_id: _Optional[int] = ..., values: _Optional[bytes] = ...) -> None: ...

class StopSensorCommand(_message.Message):
    __slots__ = ("sensor_id",)
    SENSOR_ID_FIELD_NUMBER: _ClassVar[int]
    sensor_id: int
    def __init__(self, sensor_id: _Optional[int] = ...) -> None: ...

class ScenarioCommand(_message.Message):
    __slots__ = ("payload",)
    PAYLOAD_FIELD_NUMBER: _ClassVar[int]
    payload: bytes
    def __init__(self, payload: _Optional[bytes] = ...) -> None: ...

class SyncCommand(_message.Message):
    __slots__ = ()
    def __init__(self) -> None: ...

class SessionStartCommand(_message.Message):
    __slots__ = ()
    def __init__(self) -> None: ...

class SessionStopCommand(_message.Message):
    __slots__ = ()
    def __init__(self) -> None: ...

class RuntimeCommand(_message.Message):
    __slots__ = ("setup_sensor", "set_output", "stop_sensor", "scenario", "sync")
    SETUP_SENSOR_FIELD_NUMBER: _ClassVar[int]
    SET_OUTPUT_FIELD_NUMBER: _ClassVar[int]
    STOP_SENSOR_FIELD_NUMBER: _ClassVar[int]
    SCENARIO_FIELD_NUMBER: _ClassVar[int]
    SYNC_FIELD_NUMBER: _ClassVar[int]
    setup_sensor: SetupSensorCommand
    set_output: SetOutputCommand
    stop_sensor: StopSensorCommand
    scenario: ScenarioCommand
    sync: SyncCommand
    def __init__(self, setup_sensor: _Optional[_Union[SetupSensorCommand, _Mapping]] = ..., set_output: _Optional[_Union[SetOutputCommand, _Mapping]] = ..., stop_sensor: _Optional[_Union[StopSensorCommand, _Mapping]] = ..., scenario: _Optional[_Union[ScenarioCommand, _Mapping]] = ..., sync: _Optional[_Union[SyncCommand, _Mapping]] = ...) -> None: ...

class CommandEnvelope(_message.Message):
    __slots__ = ("message_id", "session_id", "device_id", "timestamp_us", "session_start", "session_stop", "runtime")
    MESSAGE_ID_FIELD_NUMBER: _ClassVar[int]
    SESSION_ID_FIELD_NUMBER: _ClassVar[int]
    DEVICE_ID_FIELD_NUMBER: _ClassVar[int]
    TIMESTAMP_US_FIELD_NUMBER: _ClassVar[int]
    SESSION_START_FIELD_NUMBER: _ClassVar[int]
    SESSION_STOP_FIELD_NUMBER: _ClassVar[int]
    RUNTIME_FIELD_NUMBER: _ClassVar[int]
    message_id: str
    session_id: str
    device_id: str
    timestamp_us: int
    session_start: SessionStartCommand
    session_stop: SessionStopCommand
    runtime: RuntimeCommand
    def __init__(self, message_id: _Optional[str] = ..., session_id: _Optional[str] = ..., device_id: _Optional[str] = ..., timestamp_us: _Optional[int] = ..., session_start: _Optional[_Union[SessionStartCommand, _Mapping]] = ..., session_stop: _Optional[_Union[SessionStopCommand, _Mapping]] = ..., runtime: _Optional[_Union[RuntimeCommand, _Mapping]] = ...) -> None: ...
