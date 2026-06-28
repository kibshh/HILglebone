from google.protobuf.internal import enum_type_wrapper as _enum_type_wrapper
from google.protobuf import descriptor as _descriptor
from typing import ClassVar as _ClassVar

DESCRIPTOR: _descriptor.FileDescriptor

class ProtocolId(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    PROTOCOL_ID_UNSPECIFIED: _ClassVar[ProtocolId]
    PROTOCOL_ID_I2C: _ClassVar[ProtocolId]
    PROTOCOL_ID_SPI: _ClassVar[ProtocolId]
    PROTOCOL_ID_DIGITAL_OUT: _ClassVar[ProtocolId]
    PROTOCOL_ID_DIGITAL_IN: _ClassVar[ProtocolId]
    PROTOCOL_ID_DAC: _ClassVar[ProtocolId]
    PROTOCOL_ID_PWM: _ClassVar[ProtocolId]
    PROTOCOL_ID_FREQ: _ClassVar[ProtocolId]
    PROTOCOL_ID_ONEWIRE: _ClassVar[ProtocolId]
    PROTOCOL_ID_CAN: _ClassVar[ProtocolId]

class Stm32State(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    STM32_STATE_UNSPECIFIED: _ClassVar[Stm32State]
    STM32_STATE_SYNCED: _ClassVar[Stm32State]
    STM32_STATE_UNSYNCED: _ClassVar[Stm32State]
    STM32_STATE_ERROR: _ClassVar[Stm32State]
PROTOCOL_ID_UNSPECIFIED: ProtocolId
PROTOCOL_ID_I2C: ProtocolId
PROTOCOL_ID_SPI: ProtocolId
PROTOCOL_ID_DIGITAL_OUT: ProtocolId
PROTOCOL_ID_DIGITAL_IN: ProtocolId
PROTOCOL_ID_DAC: ProtocolId
PROTOCOL_ID_PWM: ProtocolId
PROTOCOL_ID_FREQ: ProtocolId
PROTOCOL_ID_ONEWIRE: ProtocolId
PROTOCOL_ID_CAN: ProtocolId
STM32_STATE_UNSPECIFIED: Stm32State
STM32_STATE_SYNCED: Stm32State
STM32_STATE_UNSYNCED: Stm32State
STM32_STATE_ERROR: Stm32State
