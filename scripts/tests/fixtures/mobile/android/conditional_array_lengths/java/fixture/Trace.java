package fixture;
import java.io.IOException;
public final class Trace {
    private Trace() {}
    public static Holder holder, observed;
    public static int[] prepared, afterRead, replacement;
    public static Object target;
    public static int events, failAt, reads, constructors, value;
    public static IOException checked;
    public static RuntimeException runtime;
    public static Error error;
    public static void start() {
        events = events * 10 + 1;
        if (failAt == 1) {
            if (error != null) throw error;
            throw runtime;
        }
        if (holder != null) holder.first = prepared;
    }
    public static void hit(int event) throws IOException {
        events = events * 10 + event;
        if (event == failAt) {
            if (checked != null) throw checked;
            if (error != null) throw error;
            throw runtime;
        }
    }
    public static int read(Holder input) throws IOException {
        reads++;
        observed = input;
        hit(2);
        input.first = afterRead;
        return value;
    }
}
