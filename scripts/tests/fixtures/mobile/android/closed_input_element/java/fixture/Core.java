package fixture;
import java.util.Set;
public final class Core {
    public Core() {}
    public static Object call(Subject target, Class<? extends Factory<?>> token, Set<Class<?>> seen, int shape) {
        switch (shape) {
        case 0: return target.call(token, seen);
        case 1: return target.text(token, seen);
        case 2: return target.fresh(token);
        default: target.discard(token, seen); return null;
        }
    }
}
