package fixture;
@SuppressWarnings("rawtypes")
public final class Adapter implements Callback {
    public final Object seed,selector;public final int capturedIndex;
    public Adapter(Object seed,Object selector,int index){this.seed=seed;this.selector=selector;this.capturedIndex=index;Trace.adapter=this;Trace.hit(3);}
    public Object step(Object first,Object second,int index){Trace.callbackFirst=first;Trace.callbackSecond=second;Trace.givenIndex=index;Trace.hit(6);Trace.callbackAction.run();return Trace.result;}
}
