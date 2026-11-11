package fixture;
public final class Core implements Port {
 public int total;
 public Core apply(char value) {
  Trace.hit(0,this,value);
  total+=value;
  Trace.hit(1,this,value);
  return Trace.result;
 }
 public final void discard(char value) {apply(value);}
}
