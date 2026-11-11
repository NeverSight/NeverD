package fixture;
import java.util.Comparator;
public class Parent<T> {
    public final boolean flag;
    public final Comparator<? super T> cmp;
    public Parent(boolean flag,Comparator<? super T> cmp) {
        Trace.parents++;Trace.hit(1);this.flag=flag;this.cmp=cmp;Trace.partial=this;
        Trace.action.run();Trace.hit(2);
    }
}
