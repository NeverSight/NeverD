package fixture;
import java.util.Comparator;
public class Base<A> {
    public Base(int number,Comparator<? super A> callback) {
        Trace.bases++;
        Trace.parentNumber=number;
        Trace.parentCallback=callback;
        Trace.baseAction.run();
    }
}
