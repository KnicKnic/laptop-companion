namespace X3LaptopCompanion
{
    public sealed class CompanionConnectionStatus
    {
        public CompanionConnectionStatus(bool isConnected, string message)
        {
            IsConnected = isConnected;
            Message = message;
        }

        public bool IsConnected { get; }
        public string Message { get; }
    }
}
