import fixture.*;
public final class StaticReturnCaller {
 public static Object call(int slot,int consume,Object[] input,Box box,Factory factory,String stamp,int flags,long wide){
  if(consume!=0)return slot==0?Client.genericValue(input,box,factory,stamp,flags,wide):Client.nominalValue(input,box,factory,stamp,flags,wide);
  return slot==0?Client.generic(input,box,factory,stamp,flags,wide):Client.nominal(input,box,factory,stamp,flags,wide);
 }
}
