package fixture;
@SuppressWarnings({"unchecked","rawtypes"})
public final class Client {
 public static Object whole(Pair input){Pair<Box<Object>,Box<Object>> result=Provider.adapt(input);Trace.after=result;Trace.hit(5);return result;}
 public static String first(Pair input){Pair<Box<String>,Box<Object>> result=Provider.adapt(input);Trace.after=result;Trace.hit(5);return result.left.value;}
 public static Object tail(Pair input){Pair<Box<Object>,Box<Object>> result=Provider.adapt(input);Trace.after=result;Trace.hit(5);return result.right.value;}
}
