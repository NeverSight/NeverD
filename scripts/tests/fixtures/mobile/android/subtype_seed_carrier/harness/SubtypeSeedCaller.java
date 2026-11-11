import fixture.*;
@SuppressWarnings("rawtypes")
public final class SubtypeSeedCaller {
    public static Result pick(Producer source, Object seed, Context context, long flags, int bits) {
        return Client.pick(source, seed, context, flags, bits);
    }
}
