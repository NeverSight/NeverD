package fixture;
public class Ancestor<A> {
 static { Trace.hit(9); }
 public int committed;
 public boolean touch(Object value) {
  Trace.receiver=this;Trace.seen=value;Trace.hit(2);committed=Trace.commit;
  Trace.hit(3);return Trace.answer;
 }
}
