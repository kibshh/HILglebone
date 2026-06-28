from hilglebone.v1 import common_pb2 as _common_pb2
from google.protobuf.internal import enum_type_wrapper as _enum_type_wrapper
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class OnlineStatus(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    ONLINE_STATUS_UNSPECIFIED: _ClassVar[OnlineStatus]
    ONLINE_STATUS_ONLINE: _ClassVar[OnlineStatus]
    ONLINE_STATUS_BUSY: _ClassVar[OnlineStatus]
    ONLINE_STATUS_OFFLINE: _ClassVar[OnlineStatus]
ONLINE_STATUS_UNSPECIFIED: OnlineStatus
ONLINE_STATUS_ONLINE: OnlineStatus
ONLINE_STATUS_BUSY: OnlineStatus
ONLINE_STATUS_OFFLINE: OnlineStatus

class StatusEnvelope(_message.Message):
    __slots__ = ("device_id", "session_id", "timestamp_us", "online_status", "stm32_state")
    DEVICE_ID_FIELD_NUMBER: _ClassVar[int]
    SESSION_ID_FIELD_NUMBER: _ClassVar[int]
    TIMESTAMP_US_FIELD_NUMBER: _ClassVar[int]
    ONLINE_STATUS_FIELD_NUMBER: _ClassVar[int]
    STM32_STATE_FIELD_NUMBER: _ClassVar[int]
    device_id: str
    session_id: str
    timestamp_us: int
    online_status: OnlineStatus
    stm32_state: _common_pb2.Stm32State
    def __init__(self, device_id: _Optional[str] = ..., session_id: _Optional[str] = ..., timestamp_us: _Optional[int] = ..., online_status: _Optional[_Union[OnlineStatus, str]] = ..., stm32_state: _Optional[_Union[_common_pb2.Stm32State, str]] = ...) -> None: ...
