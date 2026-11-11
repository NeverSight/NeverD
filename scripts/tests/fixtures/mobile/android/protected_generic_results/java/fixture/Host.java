package fixture;
public class Host {
    public static Base<Fn<? super Throwable, ? extends Throwable>> receiver =
            new Base<Fn<? super Throwable, ? extends Throwable>>() {
                @Override protected Fn<Throwable, Throwable> compute(Class<?> type) {
                    State.calls++;
                    State.type = type;
                    if (type == null) throw new NullPointerException("owned-key");
                    if (State.failure != null) throw State.failure;
                    State.last = State.value;
                    return State.value;
                }
            };
}
