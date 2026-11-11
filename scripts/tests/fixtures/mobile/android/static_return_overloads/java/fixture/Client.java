package fixture;
public final class Client {
 public static Object generic(Object[] input,Box box,Factory factory,String stamp,int flags,long wide){
  Object value=Core.genericSlot(input,box,factory,stamp,flags,wide);
  Trace.after=value;Trace.hit(8);return value;
 }
 public static Object nominal(Object[] input,Box box,Factory factory,String stamp,int flags,long wide){
  Box value=Core.nominalSlot(input,box,factory,stamp,flags,wide);
  Trace.after=value;Trace.hit(9);return value;
 }
 public static String genericValue(Object[] input,Box box,Factory factory,String stamp,int flags,long wide){
  Object value=generic(input,box,factory,stamp,flags,wide);return (String)value;
 }
 public static String nominalValue(Object[] input,Box box,Factory factory,String stamp,int flags,long wide){
  Box<String> value=(Box<String>)nominal(input,box,factory,stamp,flags,wide);return value.value;
 }
}
