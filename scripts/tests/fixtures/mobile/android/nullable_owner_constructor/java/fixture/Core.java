package fixture; public final class Core<S> { public final State<S> state;public final Core<?> parent;public final String label;
 public Core(State<S> state,Core<?> parent,String label){Trace.partial=this;Trace.hit(2);this.state=state;Trace.hit(3);this.parent=parent;this.label=label;Trace.hit(4);}
 public Core(Mutable<S> input,String label){Trace.check(input);this((State<S>)input,null,label);}
 public Core(State<S> input,String label){this(input,null,label);}
}
