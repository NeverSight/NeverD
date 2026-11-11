package fixture;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public class AnonymousSuperTokenResultHarness {
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned anonymous super token result mismatch");
    }

    private static final class Observer extends Trace {
        final IOException io = new IOException("owned IO");
        final ClassNotFoundException missing = new ClassNotFoundException("owned missing");
        final IllegalStateException runtime = new IllegalStateException("owned runtime");
        final List<Integer> events = new ArrayList<Integer>();
        final Host host;
        final Base receiver;
        final Input argument;
        final Class<?> initial, selected, fallback;
        final int mode;

        Observer(Host host, Base receiver, Input argument, Class<?> initial,
                 Class<?> selected, Class<?> fallback, int mode) {
            this.host = host;
            this.receiver = receiver;
            this.argument = argument;
            this.initial = initial;
            this.selected = selected;
            this.fallback = fallback;
            this.mode = mode;
        }

        @Override public void observe(int phase, Object owner, Input input, Object value)
                throws IOException, ClassNotFoundException {
            events.add(phase);
            require(owner == receiver && input == argument && host.calls == 1);
            if (phase == 1) {
                require(value == host && (input == null || input.token == initial));
            } else if (phase == 2) {
                require(input != null && value == null && input.token == initial);
                input.token = selected;
            } else if (phase == 3) {
                require(input != null && input.token == selected && value == selected);
                input.token = Long[].class;
            } else if (phase == 4) {
                require(input != null && input.token == Long[].class && value == selected);
            } else if (phase == 5) {
                require(input == null && value == fallback && host.fallback == fallback);
                host.fallback = Double[].class;
            } else {
                require(false);
            }
            if (mode == phase && mode >= 1 && mode <= 4) throw io;
            if (mode == 5 && phase == 2) throw missing;
            if (mode == 6 && phase == 5) throw runtime;
        }
    }

    public static void main(String[] args) {
        int shape = Integer.parseInt(args[0]);
        int mode = Integer.parseInt(args[1]);
        Class<?>[] tokens = {null, null, String.class, Integer.class, int.class, Object[].class};
        for (int attempt = 0; attempt < 2; ++attempt) {
            Host host = new Host();
            host.fallback = Character[].class;
            Input input = shape == 0 ? null : new Input();
            Class<?> initial = tokens[shape];
            Class<?> selected = shape == 3 ? Double.class : initial;
            if (input != null) input.token = initial;
            Base receiver = host.make();
            Observer observer = new Observer(host, receiver, input, initial, selected, host.fallback, mode);
            Trace.current = observer;
            Object result = null;
            Throwable caught = null;
            try {
                result = receiver.read(input);
            } catch (Throwable failure) {
                caught = failure;
            } finally {
                Trace.current = null;
            }
            List<Integer> full = shape == 0 ? Arrays.asList(1, 5) : Arrays.asList(1, 2, 3, 4);
            int failedPhase = mode >= 1 && mode <= 4 ? mode : mode == 5 ? 2 : mode == 6 ? 5 : -1;
            int stop = full.indexOf(failedPhase);
            boolean failed = stop >= 0;
            require(observer.events.equals(failed ? full.subList(0, stop + 1) : full));
            Throwable expected = mode == 5 ? observer.missing : mode == 6 ? observer.runtime : observer.io;
            require(caught == (failed ? expected : null));
            require(result == (failed ? null : input == null ? observer.fallback : selected));
            require(host.calls == 1);
            if (input != null) {
                Class<?> expectedToken = observer.events.contains(3) ? Long[].class
                        : observer.events.contains(2) ? selected : initial;
                require(input.token == expectedToken);
            }
            require(host.fallback == (observer.events.contains(5) ? Double[].class : observer.fallback));
        }
        System.out.println("attempts=2,anonymous-super-token-result=matched");
    }
}
