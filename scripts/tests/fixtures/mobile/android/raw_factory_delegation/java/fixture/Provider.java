package fixture;
public final class Provider {
 static {Trace.hit(1);}
 public static <U> Box<U> make(){Trace.hit(2);Box<U> value=(Box<U>)Trace.reply;if(Trace.mutate)value.value=(U)Trace.replacement;return value;}
}
