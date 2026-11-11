import fixture.Base;
import fixture.Broad;
public final class ProtectedGetterCaller {
    public static Broad<?> call(Base<?> receiver) { return receiver.dispatch(); }
}
