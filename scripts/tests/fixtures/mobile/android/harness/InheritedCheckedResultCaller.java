import fixture.*;
@SuppressWarnings("rawtypes")
public final class InheritedCheckedResultCaller {
    public static Value zero(Core self) { return self.fetch(); }
    public static Value indexed(Core self, int index) { return self.fetch(index); }
    public static Value wide(Core self, long stamp, double fraction, Object token) {
        return self.fetch(stamp, fraction, token);
    }
    public static Object raw(Parent self, int index) { return self.fetch(index); }
    public static Value nested(Host.Nested self, int index) { return self.fetch(index); }
}
