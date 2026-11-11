import fixture.Parent;
import java.util.function.Consumer;
public final class ClosedCallbackDefaultProbe implements Consumer<Parent> {
    public int calls;
    public Parent received;
    public Throwable failure;
    public void accept(Parent receiver) {
        calls++;received=receiver;
        if(failure!=null) { if(failure instanceof RuntimeException)throw (RuntimeException)failure;throw (Error)failure; }
    }
}
