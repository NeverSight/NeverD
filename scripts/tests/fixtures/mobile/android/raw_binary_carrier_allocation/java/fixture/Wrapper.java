package fixture;
import java.util.Comparator;
public final class Wrapper implements Comparator {
    public final Comparator callback;
    public Wrapper(Comparator callback) {
        Trace.wrappers++;
        Trace.wrapper=this;
        Trace.wrapperStage=1;
        Trace.wrapperAction.run();
        this.callback=callback;
        Trace.wrapperStage=2;
    }
    public int compare(Object left,Object right) {
        Trace.comparisons++;
        return Trace.answer;
    }
}
