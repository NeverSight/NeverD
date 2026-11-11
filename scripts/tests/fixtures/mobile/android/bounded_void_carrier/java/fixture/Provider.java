package fixture;
import api.*;
public class Provider {
    public static <T extends Bound> void work(Function<? super Context,? extends T> factory,Marker first,
            Function<? super T,Unit> reset,Function<? super T,Unit> release,Function<? super T,Unit> update,
            Marker second,int flags,int mask) {
        Trace.factory=factory;Trace.first=first;Trace.reset=reset;Trace.release=release;Trace.update=update;Trace.second=second;Trace.flags=flags;Trace.mask=mask;Trace.hit(1);
        T value=factory.invoke(Trace.context);Trace.created=value;Trace.hit(2);
        Trace.resetResult=reset.invoke(value);Trace.hit(3);
        Trace.releaseResult=release.invoke(value);Trace.hit(4);
        Trace.updateResult=update.invoke(value);Trace.hit(5);
        Trace.done=true;Trace.hit(6);
    }
}
