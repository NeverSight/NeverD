package fixture;
public class Factory implements Maker {
 public <R extends Carrier<? extends Element>> R make(String label, java.io.OutputStream sink) throws java.io.IOException {
  Trace.receiver = this;
  Trace.label = label;
  Trace.sink = sink;
  Trace.hit(1);
  Trace.action.run(label, sink);
  Trace.hit(3);
  return (R) Trace.result;
 }
 public <T extends Carrier<? extends Element>> T from(String label) throws java.io.IOException {
  return (T) make(label, null);
 }
}
