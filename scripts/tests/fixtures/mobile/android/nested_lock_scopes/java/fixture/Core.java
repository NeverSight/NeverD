package fixture;
public final class Core {
 public static void run(Object outer,Object inner,Fn callback){
  callback.call(1);
  try {
   synchronized(outer){
    callback.call(2);
    try {synchronized(inner){callback.call(3);Trace.value++;callback.call(4);}}
    finally {callback.call(5);}
   }
  } finally {callback.call(6);}
  callback.call(7);
 }
}
