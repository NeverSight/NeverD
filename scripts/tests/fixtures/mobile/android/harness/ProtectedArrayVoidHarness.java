import fixture.Base;
import fixture.Passive;
import fixture.Subject;
import fixture.Trace;
import java.util.ArrayList;
import java.util.List;

public final class ProtectedArrayVoidHarness {
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned protected array void mismatch");
    }

    private static final class Observer extends Trace {
        final RuntimeException failure = new IllegalArgumentException("owned failure");
        final Error error = new LinkageError("owned error");
        final List<String> events = new ArrayList<String>();
        final Base receiver;
        final Object[] input, previousSaved;
        final Object previousSeen;
        final int previousWrites, marker, mode, offset;
        final Object[] initial, callback, written;

        Observer(Base receiver, Object[] input, int marker, int mode, boolean derived) {
            this.receiver = receiver;
            this.input = input;
            this.marker = marker;
            this.mode = mode;
            this.offset = derived ? 4 : 0;
            this.previousSaved = receiver.saved;
            this.previousSeen = receiver.seen;
            this.previousWrites = receiver.writes;
            this.initial = input == null ? null : input.clone();
            this.callback = new Object[]{new String("callback zero"), new String("callback one")};
            this.written = new Object[]{new String("written zero"), mode == 7 ? Integer.valueOf(13) : new String("written one")};
        }

        @Override public void observe(int phase, Base owner, Object[] array, int index, Object value) {
            require(owner == receiver && owner.marker == marker);
            events.add(phase + ":" + index);
            if (phase == 0) {
                require(array == null && index == -1 && value == null);
                require(owner.saved == previousSaved && owner.seen == previousSeen && owner.writes == previousWrites);
                if (mode == 1) throw failure;
                return;
            }
            require(array == input);
            if (phase == 1 + offset) {
                require(index == -1 && value == null);
                require(owner.saved == previousSaved && owner.seen == previousSeen && owner.writes == previousWrites);
                if (mode == 2) throw error;
            } else if (phase == 2 + offset) {
                require(index >= 0 && index < 2 && input != null);
                require(owner.saved == input && owner.seen == initial[index] && value == initial[index]);
                require(owner.writes == previousWrites + index && input[index] == initial[index]);
                if (index != 0) require(input[0] == written[0]);
                input[index] = callback[index];
                Trace.value = written[index];
                if ((mode == 3 && index == 0) || (mode == 5 && index == 1)) throw failure;
            } else if (phase == 3 + offset) {
                require(index >= 0 && index < 2 && input != null);
                require(owner.saved == input && owner.seen == initial[index] && value == initial[index]);
                require(input[index] == written[index] && owner.writes == previousWrites + index + 1);
                Trace.value = new Object();
                if (mode == 4 && index == 0) throw failure;
                if (mode == 6 && index == 1) throw error;
            } else {
                require(phase == 4 + offset && index == -1 && value == null);
                require(owner.saved == input && owner.writes == previousWrites + input.length);
                require(owner.seen == (input.length == 0 ? previousSeen : initial[1]));
            }
        }
    }

    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]);
        int shape = Integer.parseInt(args[1]);
        int mode = Integer.parseInt(args[2]);
        boolean derived = Integer.parseInt(args[3]) != 0;
        Passive receiver = derived ? new Subject() : new Passive();
        receiver.saved = new Object[]{new Object()};
        receiver.seen = new Object();
        receiver.writes = 37;
        for (int attempt = 0; attempt < 2; ++attempt) {
            String left = new String("left " + attempt), right = new String("right " + attempt);
            Object[] input = shape == 0 ? null : shape == 1 ? new Object[0]
                    : shape == 2 ? new Object[]{left, right}
                    : shape == 3 ? new String[]{left, right} : new CharSequence[]{left, right};
            int marker = 19 + attempt * 7;
            Observer observer = new Observer(receiver, input, marker, mode, derived);
            Trace.value = new Object();
            Trace.hook = observer;
            Throwable caught = null;
            try {
                if (route == 0) receiver.write(input, marker);
                else if (route == 1) receiver.forward(input, marker);
                else throw new AssertionError("unknown owned route");
            } catch (Throwable problem) {
                caught = problem;
            } finally {
                Trace.hook = null;
            }
            List<String> expected = new ArrayList<String>();
            expected.add("0:-1");
            boolean entered = mode != 1;
            boolean saved = entered && mode != 2;
            boolean loop = saved && shape >= 2;
            int offset = derived ? 4 : 0;
            if (entered) expected.add((1 + offset) + ":-1");
            int beforeCount = 0, afterCount = 0;
            if (loop) {
                expected.add((2 + offset) + ":0");
                beforeCount = 1;
                if (mode != 3) {
                    expected.add((3 + offset) + ":0");
                    afterCount = 1;
                    if (mode != 4) {
                        expected.add((2 + offset) + ":1");
                        beforeCount = 2;
                        if (mode != 5 && !(mode == 7 && shape >= 3)) {
                            expected.add((3 + offset) + ":1");
                            afterCount = 2;
                        }
                    }
                }
            }
            boolean complete = saved && (shape == 1 || (loop && afterCount == 2 && mode != 6));
            if (complete) expected.add((4 + offset) + ":-1");
            require(observer.events.equals(expected));
            if (mode == 1 || (loop && mode >= 3 && mode <= 5)) require(caught == observer.failure);
            else if (mode == 2 || (loop && mode == 6)) require(caught == observer.error);
            else if (saved && shape == 0) require(caught != null && caught.getClass() == NullPointerException.class);
            else if (loop && mode == 7 && shape >= 3) require(caught != null && caught.getClass() == ArrayStoreException.class);
            else require(caught == null);
            require(receiver.marker == marker && receiver.saved == (saved ? input : observer.previousSaved));
            require(receiver.writes == observer.previousWrites + afterCount);
            require(receiver.seen == (beforeCount == 0 ? observer.previousSeen : observer.initial[beforeCount - 1]));
            if (input != null) {
                for (int i = 0; i < input.length; ++i) {
                    Object wanted = i < afterCount ? observer.written[i]
                            : i < beforeCount ? observer.callback[i] : observer.initial[i];
                    require(input[i] == wanted);
                }
            }
        }
        System.out.println("attempts=2,protected-array-void=matched");
    }
}
