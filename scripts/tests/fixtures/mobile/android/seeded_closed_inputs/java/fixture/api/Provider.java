package fixture.api;
import fixture.*;
class Provider {
    static { Trace.hit(1); }
    public static <T> Box<T> make(T seed, Box<T> input,
            Fn<? super Label,Label> closed, Fn<? super T,Label> callback,
            Producer<Label> producer) {
        Trace.seed=seed; Trace.input=input; Trace.hit(2);
        Label produced=producer.get(); Trace.produced=produced; Trace.hit(4);
        Label first=closed.apply(produced); Trace.first=first; Trace.hit(6);
        Label second=callback.apply(seed); Trace.second=second; Trace.hit(8);
        return (Box<T>)Trace.box;
    }
}
