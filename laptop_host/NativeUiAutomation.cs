using System;
using System.Runtime.InteropServices;
using System.Windows.Automation;

namespace X3LaptopCompanion
{
    internal static class NativeUiAutomation
    {
        private const int RuntimeIdPropertyId = 30000;
        private const int FullDescriptionPropertyId = 30159;

        // The managed UIA API does not expose FullDescription. Match the same live element
        // by runtime ID through native UIA, without using screen coordinates or moving focus.
        public static string ReadFullDescription(IntPtr windowHandle, int[] runtimeId)
        {
            if (windowHandle == IntPtr.Zero)
            {
                throw new ArgumentException("A window handle is required.", nameof(windowHandle));
            }

            if (runtimeId == null || runtimeId.Length == 0)
            {
                throw new ArgumentException("An element runtime ID is required.", nameof(runtimeId));
            }

            var automation = (IUIAutomation)new CUIAutomation();
            IUIAutomationElement root = null;
            IUIAutomationCondition condition = null;
            IUIAutomationElement element = null;
            try
            {
                root = automation.ElementFromHandle(windowHandle);
                if (root == null)
                {
                    throw new ElementNotAvailableException("The meeting window is no longer available.");
                }

                condition = automation.CreatePropertyCondition(RuntimeIdPropertyId, runtimeId);
                element = root.FindFirst(TreeScope.Descendants, condition);
                if (element == null)
                {
                    throw new ElementNotAvailableException("The button disappeared before its description could be read.");
                }

                var value = element.GetCurrentPropertyValue(FullDescriptionPropertyId);
                if (value is string description)
                {
                    return description;
                }

                throw new NotSupportedException("The button does not expose a UIA FullDescription string.");
            }
            finally
            {
                if (element != null) Marshal.ReleaseComObject(element);
                if (condition != null) Marshal.ReleaseComObject(condition);
                if (root != null) Marshal.ReleaseComObject(root);
                Marshal.ReleaseComObject(automation);
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct NativePoint
        {
            public int X;
            public int Y;
        }

        [ComImport, Guid("FF48DBA4-60EF-4201-AA87-54103EEF594E")]
        private class CUIAutomation
        {
        }

        [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("352FFBA8-0973-437C-A61F-F64CAFD81DF9")]
        private interface IUIAutomationCondition
        {
        }

        // Declare the leading native slots in order, including the unused methods between
        // ElementFromHandle and CreatePropertyCondition, to preserve the COM vtable layout.
        [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("30CBE57D-D9D0-452A-AB13-7AC5AC4825EE")]
        private interface IUIAutomation
        {
            int CompareElements(IUIAutomationElement first, IUIAutomationElement second);
            int CompareRuntimeIds(
                [MarshalAs(UnmanagedType.SafeArray, SafeArraySubType = VarEnum.VT_I4)] int[] first,
                [MarshalAs(UnmanagedType.SafeArray, SafeArraySubType = VarEnum.VT_I4)] int[] second);
            IUIAutomationElement GetRootElement();
            IUIAutomationElement ElementFromHandle(IntPtr hwnd);
            IUIAutomationElement ElementFromPoint(NativePoint point);
            IUIAutomationElement GetFocusedElement();
            IUIAutomationElement GetRootElementBuildCache(IntPtr cacheRequest);
            IUIAutomationElement ElementFromHandleBuildCache(IntPtr hwnd, IntPtr cacheRequest);
            IUIAutomationElement ElementFromPointBuildCache(NativePoint point, IntPtr cacheRequest);
            IUIAutomationElement GetFocusedElementBuildCache(IntPtr cacheRequest);
            IntPtr CreateTreeWalker(IUIAutomationCondition condition);
            IntPtr GetControlViewWalker();
            IntPtr GetContentViewWalker();
            IntPtr GetRawViewWalker();
            IUIAutomationCondition GetRawViewCondition();
            IUIAutomationCondition GetControlViewCondition();
            IUIAutomationCondition GetContentViewCondition();
            IntPtr CreateCacheRequest();
            IUIAutomationCondition CreateTrueCondition();
            IUIAutomationCondition CreateFalseCondition();
            IUIAutomationCondition CreatePropertyCondition(int propertyId, [MarshalAs(UnmanagedType.Struct)] object value);
        }

        [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("D22108AA-8AC5-49A5-837B-37BBB3D7591E")]
        private interface IUIAutomationElement
        {
            void SetFocus();
            [return: MarshalAs(UnmanagedType.SafeArray, SafeArraySubType = VarEnum.VT_I4)]
            int[] GetRuntimeId();
            IUIAutomationElement FindFirst(TreeScope scope, IUIAutomationCondition condition);
            IntPtr FindAll(TreeScope scope, IUIAutomationCondition condition);
            IUIAutomationElement FindFirstBuildCache(TreeScope scope, IUIAutomationCondition condition, IntPtr cacheRequest);
            IntPtr FindAllBuildCache(TreeScope scope, IUIAutomationCondition condition, IntPtr cacheRequest);
            IUIAutomationElement BuildUpdatedCache(IntPtr cacheRequest);
            [return: MarshalAs(UnmanagedType.Struct)]
            object GetCurrentPropertyValue(int propertyId);
        }
    }
}
