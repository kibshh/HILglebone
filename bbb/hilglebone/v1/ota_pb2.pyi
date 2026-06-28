from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from typing import ClassVar as _ClassVar, Optional as _Optional

DESCRIPTOR: _descriptor.FileDescriptor

class OtaEnvelope(_message.Message):
    __slots__ = ("message_id", "session_id", "device_id", "timestamp_us", "firmware_url", "firmware_sha256", "firmware_size")
    MESSAGE_ID_FIELD_NUMBER: _ClassVar[int]
    SESSION_ID_FIELD_NUMBER: _ClassVar[int]
    DEVICE_ID_FIELD_NUMBER: _ClassVar[int]
    TIMESTAMP_US_FIELD_NUMBER: _ClassVar[int]
    FIRMWARE_URL_FIELD_NUMBER: _ClassVar[int]
    FIRMWARE_SHA256_FIELD_NUMBER: _ClassVar[int]
    FIRMWARE_SIZE_FIELD_NUMBER: _ClassVar[int]
    message_id: str
    session_id: str
    device_id: str
    timestamp_us: int
    firmware_url: str
    firmware_sha256: str
    firmware_size: int
    def __init__(self, message_id: _Optional[str] = ..., session_id: _Optional[str] = ..., device_id: _Optional[str] = ..., timestamp_us: _Optional[int] = ..., firmware_url: _Optional[str] = ..., firmware_sha256: _Optional[str] = ..., firmware_size: _Optional[int] = ...) -> None: ...
