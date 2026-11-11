package fixture;
import java.util.concurrent.atomic.AtomicReference;
import kotlin.jvm.functions.Function1;
import kotlin.jvm.functions.Function2;
import kotlin.coroutines.Continuation;
public final class Client {
    private Client() {}
    public static Object send(AtomicReference state,Function1 factory,Function2 action,Continuation frame) {
        return Carrier.process(state,factory,action,frame);
    }
}
