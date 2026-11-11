package fixture;
public class Receiver implements Fixed {
    public int dispatch(Fixed other) {
        Trace.self=this;
        Trace.other=other;
        Trace.hit(1);
        Trace.action.run();
        Trace.hit(2);
        if (Trace.invoke) Trace.observed=other.guard(Trace.word);
        Trace.hit(3);
        return Trace.result;
    }
    public long guard(long value) {
        Trace.guardSelf=this;
        Trace.guardArg=value;
        Trace.hit(4);
        Trace.guardAction.run();
        Trace.hit(5);
        return Trace.reply;
    }
}
