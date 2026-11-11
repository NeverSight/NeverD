import fixture.Base;
import fixture.Marker;
import fixture.Passive;
import fixture.Subject;
import fixture.Trace;
import java.util.ArrayList;
import java.util.List;

public final class ProtectedMarkerVoidHarness {
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned protected marker void mismatch");
    }

    private static final class Value implements Marker {}

    private static final class Observer extends Trace {
        final RuntimeException failure = new IllegalArgumentException("owned failure");
        final Error error = new LinkageError("owned error");
        final List<Integer> events = new ArrayList<Integer>();
        final Base receiver, context, oldContext;
        final Marker input, oldSaved;
        final Object oldSeen, callbackValue = new Object();
        final int tag, mode, offset, oldWrites;

        Observer(Base receiver, Marker input, Base context, int tag, int mode, boolean derived) {
            this.receiver = receiver;
            this.input = input;
            this.context = context;
            this.tag = tag;
            this.mode = mode;
            this.offset = derived ? 4 : 0;
            this.oldSaved = receiver.saved;
            this.oldContext = receiver.context;
            this.oldSeen = context == null ? null : context.seen;
            this.oldWrites = receiver.writes;
        }

        @Override public void observe(int phase, Base owner, Marker value, Base target) {
            require(owner == receiver && owner.marker == tag);
            events.add(Integer.valueOf(phase));
            if (phase == 0) {
                require(value == null && target == null);
                require(owner.saved == oldSaved && owner.context == oldContext && owner.writes == oldWrites);
                if (mode == 1) throw failure;
                return;
            }
            require(value == input && target == context);
            if (phase == 1 + offset) {
                require(owner.saved == oldSaved && owner.context == oldContext && owner.writes == oldWrites);
                if (context != null) require(context.seen == oldSeen);
                if (mode == 2) throw error;
            } else if (phase == 2 + offset) {
                require(owner.saved == input && owner.context == context && owner.writes == oldWrites);
                if (context != null) {
                    require(context.seen == oldSeen);
                    context.seen = callbackValue;
                }
                if (mode == 3) throw failure;
            } else if (phase == 3 + offset) {
                require(context != null && context.seen == input);
                require(owner.saved == input && owner.context == context && owner.writes == oldWrites + 1);
                if (mode == 4) throw error;
            } else {
                require(phase == 4 && context != null && context.seen == input);
                require(owner.saved == input && owner.context == context && owner.writes == oldWrites + 1);
                if (mode == 5) throw failure;
            }
        }
    }

    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]);
        int shape = Integer.parseInt(args[1]);
        int mode = Integer.parseInt(args[2]);
        boolean derived = Integer.parseInt(args[3]) != 0;
        Passive receiver = derived ? new Subject() : new Passive();
        receiver.saved = new Value();
        receiver.context = new Passive();
        receiver.writes = 23;
        for (int attempt = 0; attempt < 2; ++attempt) {
            Marker input = shape % 2 == 0 ? null : new Value();
            Base context = shape / 2 == 0 ? null : shape / 2 == 1 ? new Passive() : receiver;
            if (context != null) context.seen = new Object();
            int tag = 17 + 11 * attempt;
            Observer observer = new Observer(receiver, input, context, tag, mode, derived);
            Trace.hook = observer;
            Throwable caught = null;
            try {
                if (route == 0) receiver.write(input, context, tag);
                else receiver.forward(input, context, tag);
            } catch (Throwable failure) {
                caught = failure;
            }
            List<Integer> expected = new ArrayList<Integer>();
            expected.add(Integer.valueOf(0));
            if (mode != 1) {
                expected.add(Integer.valueOf(1 + observer.offset));
                if (mode != 2) {
                    expected.add(Integer.valueOf(2 + observer.offset));
                    if (mode != 3 && context != null) {
                        expected.add(Integer.valueOf(3 + observer.offset));
                        if (mode != 4) expected.add(Integer.valueOf(4));
                    }
                }
            }
            require(observer.events.equals(expected));
            if (mode == 1 || mode == 3 || (mode == 5 && context != null)) require(caught == observer.failure);
            else if (mode == 2 || (mode == 4 && context != null)) require(caught == observer.error);
            else if (context == null) require(caught != null && caught.getClass() == NullPointerException.class);
            else require(caught == null);
            boolean stored = mode != 1 && mode != 2;
            boolean written = stored && mode != 3 && context != null;
            require(receiver.marker == tag);
            require(receiver.saved == (stored ? input : observer.oldSaved));
            require(receiver.context == (stored ? context : observer.oldContext));
            require(receiver.writes == observer.oldWrites + (written ? 1 : 0));
            if (context != null) require(context.seen == (written ? input : stored ? observer.callbackValue : observer.oldSeen));
        }
        System.out.println("attempts=2,protected-marker-void=matched");
    }
}
