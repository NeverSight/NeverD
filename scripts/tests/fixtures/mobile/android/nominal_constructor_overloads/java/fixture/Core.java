package fixture;
public final class Core<V extends Bound> {
 public final Input input;public final float first,second;
 private Core(float first,float second,Input input){Trace.partial=this;Trace.hit(2);this.input=input;Trace.hit(3);this.first=first;this.second=second;Trace.hit(4);}
 public Core(float first,float second,V seed){this(first,second,Trace.create(seed,first,second));}
}
