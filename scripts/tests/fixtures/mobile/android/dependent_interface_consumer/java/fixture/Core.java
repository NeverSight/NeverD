package fixture;
public final class Core {
 public static <M extends Mapper<Input,Output>,Input,Output> Object use(Input value,M mapper,Context context){
  Trace.record(0,value,mapper,context,null);
  Object result=mapper.apply(context,value);
  Trace.record(1,value,mapper,context,result);
  return result;
 }
 @SuppressWarnings("unchecked") public static Object call(Object value,Mapper mapper,Context context){return use(value,mapper,context);}
}
