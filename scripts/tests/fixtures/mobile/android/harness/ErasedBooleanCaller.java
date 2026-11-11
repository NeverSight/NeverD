import fixture.*;
public final class ErasedBooleanCaller {
    public static boolean direct(Root receiver, int key, Object value) { return Client.direct(receiver, key, value); }
    public static boolean checked(Creator creator, int key, long flags, double fraction) { return Client.checked(creator, key, flags, fraction); }
}
