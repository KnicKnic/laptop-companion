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
        public static readonly Guid HostDesktopStateUuid = Guid.Parse("7d2d5f00-778d-4df6-a6d5-7c4e7a00000c");

        public const byte ProtocolVersion = 4;
        public const ushort StateCounterMask = 0x3FFF;

        public const int DesktopMaxCount = 8;
        public const int DesktopNameMaxLength = 16;
        public const int DesktopStateHeaderLength = 5;
        public const byte DesktopIndexUnknown = 0xFF;
        public const byte DesktopFlagRemote = 0x01;
        public const byte DesktopFlagDisconnected = 0x02;
        public const ushort DesktopAckNone = 0;
        public const int ButtonEventPayloadLength = 10;

        public static ushort EncodeState(bool on, ushort counter, bool locked = false)
        {
            return (ushort)(((counter & StateCounterMask) << 2) | (locked ? 0x0002 : 0) | (on ? 1 : 0));
        }

        public static bool TriStateIsOn(CompanionTriState state)
        {
            return state == CompanionTriState.On;
        }

        // Encodes [version][count][activeIndex][ackSeqLo][ackSeqHi] then, per desktop,
        // [flags][nameLen][UTF-8 name].
        public static byte[] EncodeDesktopState(CompanionDesktopState state, int maxPayloadBytes)
        {
            var desktopCount = state == null ? 0 : Math.Min(state.Desktops.Count, DesktopMaxCount);

            // Which desktops exist, which is active, and which are offline all matter more than
            // full names, so shorten names progressively instead of dropping tiles.
            for (var nameCap = DesktopNameMaxLength; nameCap >= 0; nameCap--)
            {
                var payload = TryEncodeDesktopState(state, maxPayloadBytes, nameCap);
                if (payload[1] >= desktopCount)
                {
                    return payload;
                }
            }

            return TryEncodeDesktopState(state, maxPayloadBytes, 0);
        }

        private static byte[] TryEncodeDesktopState(CompanionDesktopState state, int maxPayloadBytes, int nameCap)
        {
            var limit = Math.Max(DesktopStateHeaderLength, maxPayloadBytes);
            var payload = new System.Collections.Generic.List<byte>(DesktopStateHeaderLength);
            var ackSequence = state == null ? DesktopAckNone : state.AcknowledgedSwitchSequence;
            payload.Add(ProtocolVersion);
            payload.Add(0);
            payload.Add(DesktopIndexUnknown);
            payload.Add((byte)(ackSequence & 0xFF));
            payload.Add((byte)((ackSequence >> 8) & 0xFF));
            if (state == null || state.Desktops.Count == 0)
            {
                return payload.ToArray();
            }

            byte written = 0;
            for (var i = 0; i < state.Desktops.Count && written < DesktopMaxCount; i++)
            {
                var desktop = state.Desktops[i];
                var nameBytes = nameCap <= 0 ? Array.Empty<byte>() : TruncateName(desktop.Name, nameCap);
                if (payload.Count + 2 + nameBytes.Length > limit)
                {
                    break;
                }

                payload.Add((byte)((desktop.IsRemote ? DesktopFlagRemote : 0) |
                    (desktop.IsDisconnected ? DesktopFlagDisconnected : 0)));
                payload.Add((byte)nameBytes.Length);
                payload.AddRange(nameBytes);
                written++;
            }

            payload[1] = written;
            payload[2] = state.ActiveIndex >= 0 && state.ActiveIndex < written
                ? (byte)state.ActiveIndex
                : DesktopIndexUnknown;
            return payload.ToArray();
        }

        private static byte[] TruncateName(string name, int maxBytes)
        {
            if (string.IsNullOrWhiteSpace(name) || maxBytes <= 0)
            {
                return Array.Empty<byte>();
            }

            var bytes = System.Text.Encoding.UTF8.GetBytes(name);
            if (bytes.Length <= maxBytes)
            {
                return bytes;
            }

            // Trim to a whole UTF-8 character so the device never renders a partial glyph.
            var length = maxBytes;
            while (length > 0 && (bytes[length] & 0xC0) == 0x80)
            {
                length--;
            }

            var truncated = new byte[length];
            Array.Copy(bytes, truncated, length);
            return truncated;
        }
    }

    public sealed class CompanionDesktopInfo
    {
        public CompanionDesktopInfo(string name, bool isRemote, bool isDisconnected = false)
        {
            Name = name ?? string.Empty;
            IsRemote = isRemote;
            IsDisconnected = isDisconnected;
        }

        public string Name { get; }
        public bool IsRemote { get; }
        public bool IsDisconnected { get; }
    }

    public sealed class CompanionDesktopState
    {
        public CompanionDesktopState(System.Collections.Generic.IReadOnlyList<CompanionDesktopInfo> desktops,
            int activeIndex, ushort acknowledgedSwitchSequence = CompanionProtocol.DesktopAckNone)
        {
            Desktops = desktops ?? Array.Empty<CompanionDesktopInfo>();
            ActiveIndex = activeIndex;
            AcknowledgedSwitchSequence = acknowledgedSwitchSequence;
        }

        public System.Collections.Generic.IReadOnlyList<CompanionDesktopInfo> Desktops { get; }
        public int ActiveIndex { get; }

        /// <summary>Sequence of the last X3 switch request the host finished handling.</summary>
        public ushort AcknowledgedSwitchSequence { get; }

        public string Describe()
        {
            var parts = new System.Collections.Generic.List<string>(Desktops.Count);
            for (var i = 0; i < Desktops.Count; i++)
            {
                parts.Add((i == ActiveIndex ? "*" : string.Empty) + (i + 1) + ":" + Desktops[i].Name +
                    (Desktops[i].IsRemote ? "(remote" + (Desktops[i].IsDisconnected ? ",offline)" : ")") : string.Empty));
            }

            return string.Join(",", parts);
        }
    }

    public enum CompanionButton : byte
    {
        ToggleMute = 1,
        ToggleHand = 2,
        ToggleCamera = 3,
        SwitchDesktop = 4,
        ReactLike = 5,
        ReactHeart = 6,
        ReactApplause = 7
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
        public CompanionButtonEvent(CompanionButton button, CompanionButtonAction action, ushort sequence,
            uint deviceUptimeMs, byte argument = 0)
        {
            Button = button;
            Action = action;
            Sequence = sequence;
            DeviceUptimeMs = deviceUptimeMs;
            Argument = argument;
        }

        public CompanionButton Button { get; }
        public CompanionButtonAction Action { get; }
        public ushort Sequence { get; }
        public uint DeviceUptimeMs { get; }

        /// <summary>Button specific payload; the target desktop index for SwitchDesktop.</summary>
        public byte Argument { get; }
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
