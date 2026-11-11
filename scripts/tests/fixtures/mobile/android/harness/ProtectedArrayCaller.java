import fixture.Base;
import fixture.Broad;
public final class ProtectedArrayCaller {
    public static Broad<?>[] call(Base<?> receiver, int length) { return receiver.dispatch(length); }
}
