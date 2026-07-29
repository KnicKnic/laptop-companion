using System;

namespace X3LaptopCompanion
{
    public static class CompanionProtocol
    {
        public static readonly Guid ServiceUuid = Guid.Parse("7d2d5f00-778d-4df6-a6d5-7c4e7a000001");
        public static readonly Guid HostTeamsStateUuid = Guid.Parse("7d2d5f00-778d-4df6-a6d5-7c4e7a000002");
        public static readonly Guid HostMicrophoneStateUuid = Guid.Parse("7d2d5f00-778d-4df6-a6d5-7c4e7a000003");
        public static readonly Guid HostCameraStateUuid = Guid.Parse("7d2d5f00-778d-4df6-a6d5-7c4e7a000004");
        public static readonly Guid HostStatusMessageUuid = Guid.Parse("7d2d5f00-778d-4df6-a6d5-7c4e7a000005");
        public static readonly Guid ButtonEventUuid = Guid.Parse("7d2d5f00-778d-4df6-a6d5-7c4e7a000006");
        public static readonly Guid DeviceInfoUuid = Guid.Parse("7d2d5f00-778d-4df6-a6d5-7c4e7a000007");
        public static readonly Guid HostMeetingStateUuid = Guid.Parse("7d2d5f00-778d-4df6-a6d5-7c4e7a000008");
        public static readonly Guid HostHandStateUuid = Guid.Parse("7d2d5f00-778d-4df6-a6d5-7c4e7a000009");
        public static readonly Guid HostMeetingNameUuid = Guid.Parse("7d2d5f00-778d-4df6-a6d5-7c4e7a00000a");
        public static readonly Guid ConnectionParticipationUuid = Guid.Parse("7d2d5f00-778d-4df6-a6d5-7c4e7a00000b");

        public const byte ProtocolVersion = 3;
        public const ushort StateCounterMask = 0x3FFF;

        public static ushort EncodeState(bool on, ushort counter, bool locked = false)
        {
            return (ushort)(((counter & StateCounterMask) << 2) | (locked ? 0x0002 : 0) | (on ? 1 : 0));
        }

        public static bool TriStateIsOn(CompanionTriState state)
        {
            return state == CompanionTriState.On;
        }
    }

    public enum CompanionButton : byte
    {
        ToggleMute = 1,
        ToggleHand = 2,
        ToggleCamera = 3
    }

    public enum CompanionButtonAction : byte
    {
        Released = 1
    }

    public enum CompanionTriState : byte
    {
        Unknown = 0,
        Off = 1,
        On = 2
    }

    public sealed class CompanionButtonEvent
    {
        public CompanionButtonEvent(CompanionButton button, CompanionButtonAction action, ushort sequence, uint deviceUptimeMs)
        {
            Button = button;
            Action = action;
            Sequence = sequence;
            DeviceUptimeMs = deviceUptimeMs;
        }

        public CompanionButton Button { get; }
        public CompanionButtonAction Action { get; }
        public ushort Sequence { get; }
        public uint DeviceUptimeMs { get; }
    }

    public sealed class CompanionParticipationEvent
    {
        public CompanionParticipationEvent(uint counter)
        {
            Counter = counter;
        }

        public uint Counter { get; }
    }
}
