import fixture.Core;
import fixture.Factory;
import java.lang.reflect.Constructor;
import java.util.AbstractList;
import java.util.List;

public final class AnchoredReifiableFactoryHarness {
    public static class Alpha { public Alpha() {} }
    public static class Beta { public Beta() {} }

    private static final class FutureContext extends AbstractList<Class<?>> {
        int calls;
        int fingerprint;
        RuntimeException failure;
        public int size() { throw new AssertionError("context must remain opaque"); }
        public Class<?> get(int index) { throw new AssertionError("context must remain opaque"); }
        public int hashCode() {
            ++calls;
            if (failure != null) throw failure;
            return fingerprint;
        }
    }

    private static void check(boolean value) {
        if (!value) throw new AssertionError("owned anchored reifiable factory");
    }

    public static void main(String[] args) throws Exception {
        int route = Integer.parseInt(args[0]);
        int mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; ++attempt) {
            boolean nullType = (mode & 1) != 0;
            boolean nullContext = (mode & 2) != 0;
            boolean hashFailure = (mode & 4) != 0;
            boolean providerFailure = (mode & 8) != 0;
            boolean nullResult = (mode & 16) != 0;
            Class<Alpha> type = nullType ? null : Alpha.class;
            FutureContext context = new FutureContext();
            context.fingerprint = 31 + attempt;
            RuntimeException hashError = new RuntimeException("owned context failure");
            RuntimeException providerError = new RuntimeException("owned provider failure");
            context.failure = hashFailure ? hashError : null;
            List<? extends Class<?>> supplied = nullContext ? null : context;
            Constructor cached = nullResult ? null : (attempt == 0 ? Alpha.class : Beta.class).getConstructor();
            Object sentinel = new Object();
            Factory.events = 0;
            Factory.savedType = null;
            Factory.savedContext = null;
            Factory.name = null;
            Factory.fingerprint = -7;
            Factory.saved = sentinel;
            Factory.replacement = cached;
            Factory.failure = providerFailure ? providerError : null;
            Constructor result = null;
            boolean success = !nullType && !nullContext && !hashFailure && !providerFailure;
            boolean returned = false;
            try {
                if (route == 0) result = Core.call(type, supplied);
                else if (route == 1) Core.discard(type, supplied);
                else if (route == 2) result = Core.<Alpha>typed(type, supplied);
                else result = Factory.create(type, supplied);
                returned = true;
                check(success);
            } catch (RuntimeException error) {
                if (nullType || nullContext) check(error instanceof NullPointerException);
                else check(error == (hashFailure ? hashError : providerError));
            }
            int events = route == 3 ? (success ? 7 : 1) : (success ? 414 : 16);
            check(Factory.events == events && returned == success);
            check(Factory.savedType == type && Factory.savedContext == supplied);
            check(nullType ? Factory.name == null : type.getName().equals(Factory.name));
            check(context.calls == (!nullType && !nullContext ? 1 : 0));
            check(Factory.fingerprint == (!nullType && !nullContext && !hashFailure ? 31 + attempt : -7));
            check(Factory.saved == (success ? cached : sentinel));
            check(result == (success && route != 1 ? cached : null));
            if (success && route != 1) {
                try {
                    Alpha value = ((Constructor<Alpha>) result).newInstance();
                    check(!nullResult && attempt == 0 && value.getClass() == Alpha.class);
                } catch (NullPointerException error) {
                    check(nullResult);
                } catch (ClassCastException error) {
                    check(!nullResult && attempt == 1);
                }
            }
            check(Factory.events == events && context.calls == (!nullType && !nullContext ? 1 : 0));
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
