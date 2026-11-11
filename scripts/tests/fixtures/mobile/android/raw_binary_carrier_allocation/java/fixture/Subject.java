package fixture;
import java.util.Comparator;
public final class Subject<E> extends Base<E> {
    public final Object value;
    public final int number;
    public final Comparator<? super E> callback;
    public Subject(int number) {
        this(number,(Comparator<? super E>)null);
        Trace.shortCalls++;
    }
    public Subject(int number,Comparator<? super E> callback) {
        super(number,new Wrapper(callback));
        Trace.calls++;
        Trace.owner=this;
        Trace.number=number;
        Trace.callback=callback;
        Trace.stage=1;
        Trace.action.run();
        this.value=Trace.value;
        this.number=number;
        this.callback=callback;
        Trace.stage=2;
    }
}
