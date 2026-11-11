package fixture;
public final class Core extends Middle implements Api {
    public Core() { super(); }
    private Void cause() { return Effects.prefix(this); }
    public Void typedDispatch(Object first, Object second) { cause(); throw new Failure(); }
    public void dispatch(Object first, Object second) { typedDispatch(first, second); }
    public Void typedSchedule(long delay, Entry<? super Throwable> entry) { cause(); throw new Failure(); }
    public void schedule(long delay, Entry entry) { typedSchedule(delay, entry); }
}
