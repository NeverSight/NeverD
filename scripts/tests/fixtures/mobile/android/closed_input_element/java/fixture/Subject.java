package fixture;
import java.util.HashSet;
import java.util.Set;
public final class Subject {
    public int state;
    public Object result;
    public Subject() {}
    @SuppressWarnings("unchecked")
    private <T> T select(Class<? extends Factory<?>> token, Set<Class<?>> seen) {
        Trace.hit(1, this, token, seen);
        if (!seen.add(token)) return (T)result;
        Class<? extends Factory<?>> next = Trace.next;
        if (next != null) {
            Trace.next = null;
            select(next, seen);
        }
        Object value = Trace.factory.create(seen);
        state++;
        result = value;
        Trace.hit(2, this, token, seen);
        return (T)value;
    }
    <T> T entry(Class<? extends Factory<?>> token) {
        return select(token, new HashSet<Class<?>>());
    }
    public Object call(Class<? extends Factory<?>> token, Set<Class<?>> seen) { return select(token, seen); }
    public String text(Class<? extends Factory<?>> token, Set<Class<?>> seen) { return this.<String>select(token, seen); }
    public Object fresh(Class<? extends Factory<?>> token) { return entry(token); }
    public void discard(Class<? extends Factory<?>> token, Set<Class<?>> seen) { select(token, seen); }
}
