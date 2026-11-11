import fixture.*;
public final class ErasedCallbackCaller {
    public static Object read(Input input, Creator creator) { return Host.Helper.erasedRead(input, creator); }
    public static Writable checked(Input input, Creator creator) { return Host.Helper.checkedRead(input, creator); }
    public static void write(Input input, Writable value, int flags) { Host.Helper.erasedWrite(input, value, flags); }
}
