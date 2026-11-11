import fixture.*;
import java.util.Set;
public final class ClosedInputElementCaller {
    public static Object call(Subject target, Class token, Set seen, int shape) {
        switch (shape) {
        case 0: return target.call(token, seen);
        case 1: return target.text(token, seen);
        case 2: return target.fresh(token);
        default: target.discard(token, seen); return null;
        }
    }
}
