package fixture;
public class Factory extends Parent<Factory> {
 public <R> R make(java.util.Map<String,Object> values) throws java.io.IOException {
  Trace.receiver = this;
  Trace.values = values;
  Trace.hit(1);
  Trace.action.run(values);
  Trace.hit(3);
  return (R) Trace.result;
 }
 public static <U> U create(java.util.Map<String,Object> values, Object context) throws java.io.IOException {
  Trace.context = context;
  return Trace.factory.make(values);
 }
 public <R> R token(Class<String> token, java.util.Map<String,Object> values) throws java.io.IOException {
  Trace.token = token;
  Trace.hit(9);
  return make(values);
 }
}
