import fixture.Base;
import fixture.Exact;
import fixture.Outer;
import fixture.Trace;
import fixture.Wide;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public class ConstructorClosedExceptionsHarness {
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned constructor closed exceptions mismatch");
    }

    private static final class Observer extends Trace {
        final IOException checked = new IOException("owned checked");
        final IllegalArgumentException unchecked = new IllegalArgumentException("owned unchecked");
        final List<Integer> events = new ArrayList<Integer>();
        final int route, mode;
        final Outer outer;
        final Object token, capture;
        Base seen;

        Observer(int route, int mode, Outer outer, Object token, Object capture) {
            this.route = route;
            this.mode = mode;
            this.outer = outer;
            this.token = token;
            this.capture = capture;
        }

        private void observe(int phase, Object self, Object value) {
            events.add(phase);
            if (phase == 0) {
                require(self == outer && value == token && parent == null && seen == null);
            } else if (phase == 3) {
                require(self == outer && value == parent && value == seen);
            } else {
                require(self instanceof Base && value == token);
                Base current = (Base) self;
                if (seen == null) seen = current;
                require(seen == current);
                if (phase == 1 || phase == 5) {
                    require(parent == null && current.saved == null && current.observer == null);
                    require(current.value() == (route == 0 ? capture : null));
                } else {
                    require(parent == current && current.saved == token && current.observer == this);
                    require(current.value() == (route == 0 ? capture : token));
                }
            }
        }

        @Override public void hit(int phase, Object self, Object value) throws IOException {
            observe(phase, self, value);
            if (phase + 1 == mode) throw checked;
        }

        @Override public void unchecked(int phase, Object self, Object value) {
            observe(phase, self, value);
            if (phase + 1 == mode) throw unchecked;
        }
    }

    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]);
        int mode = Integer.parseInt(args[1]);
        int shape = Integer.parseInt(args[2]);
        List<Integer> full = Arrays.asList(route == 0 ? new Integer[]{0, 1, 2, 3}
                : route == 3 ? new Integer[]{5, 6, 7} : new Integer[]{1, 2, 4});
        for (int attempt = 0; attempt < 2; ++attempt) {
            Object token = shape == 0 ? new Object() : null;
            Object capture = shape == 0 ? new Object() : null;
            Outer outer = new Outer(capture);
            Observer observer = new Observer(route, mode, outer, token, capture);
            Base result = null;
            Throwable caught = null;
            try {
                if (route == 0) result = outer.make(observer, token);
                else if (route == 1) result = new Exact(observer, token);
                else if (route == 2) result = new Wide(observer, token);
                else if (route == 3) result = new Exact(observer, token, 7);
                else throw new AssertionError("unknown owned route");
            } catch (Throwable failure) {
                caught = failure;
            }
            int stop = full.indexOf(mode - 1);
            boolean failed = stop >= 0;
            require(observer.events.equals(failed ? full.subList(0, stop + 1) : full));
            require(caught == (failed ? (route == 3 ? observer.unchecked : observer.checked) : null));
            require(result == (failed ? null : observer.parent));
            boolean stored = observer.events.contains(2) || observer.events.contains(6);
            require(observer.parent == (stored ? observer.seen : null));
            if (observer.seen != null) {
                require(observer.seen.saved == (stored ? token : null));
                require(observer.seen.observer == (stored ? observer : null));
                require(observer.seen.value() == (route == 0 ? capture : stored ? token : null));
            }
            if (!failed) {
                Object replacement = new Object();
                outer.captured = replacement;
                require(result.value() == (route == 0 ? replacement : token));
                require(Outer.identity(result) == result);
            }
        }
        System.out.println("attempts=2,constructor-closed-exceptions=matched");
    }
}
