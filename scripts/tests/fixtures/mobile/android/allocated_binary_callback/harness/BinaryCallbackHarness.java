import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class BinaryCallbackHarness {
 static void require(boolean value){if(!value)throw new AssertionError("owned allocated binary callback changed");}
 static Child create(int route,long id,Marker marker)throws Throwable{
  if(route==0)return new Child(id,marker);
  if(route==1)return BinaryCallbackCaller.create(id,marker);
  if(route==2){try{return Child.class.getConstructor(long.class,Marker.class).newInstance(id,marker);}catch(InvocationTargetException error){throw error.getCause();}}
  return (Child)MethodHandles.publicLookup().findConstructor(Child.class,MethodType.methodType(void.class,long.class,Marker.class)).invokeExact(id,marker);
 }
 @SuppressWarnings({"rawtypes","unchecked"})
 public static void main(String[] args)throws Throwable{
  int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
  TypeVariable<?>[] formals=Callback.class.getTypeParameters();require(formals.length==2);
  for(TypeVariable<?> formal:formals)require(formal.getBounds().length==1&&formal.getBounds()[0]==Object.class);
  ParameterizedType edge=(ParameterizedType)Callback.class.getGenericInterfaces()[0];require(edge.getRawType()==Functor.class&&edge.getActualTypeArguments()[0].equals(formals[1]));
  require(Adapter.class.getGenericInterfaces()[0]==Callback.class&&Child.class.getGenericSuperclass()==Parent.class&&Child.class.getTypeParameters().length==0);
  Type[] parameters=Parent.class.getConstructor(long.class,Marker.class,Callback.class,Callback.class).getGenericParameterTypes();
  for(int i=2;i<4;i++){ParameterizedType parameter=(ParameterizedType)parameters[i];require(parameter.getRawType()==Callback.class&&parameter.getActualTypeArguments()[0]==Object.class&&parameter.getActualTypeArguments()[1]==Token.class);}
  Trace.mode=mode;RuntimeException exact=new RuntimeException("owned constructor failure");Trace.failure=exact;
  Object sentinel=new Object();Object argument=mode==14?new Token():new Object();
  Object result=mode==12?null:mode==13?new Object():mode==14?argument:mode==15?new Marker():new Token();Trace.result=result;
  Marker marker=mode==10?null:new Marker();long id=mode==11?Long.MIN_VALUE:0x1234567887654321L;
  for(int attempt=0;attempt<2;attempt++){
   Trace.events=0;Trace.parent=null;Trace.child=null;Trace.adapter=null;Trace.after=Trace.seen=Trace.arg=sentinel;
   Object actual=null;Throwable failure=null;
   try{Child child=create(route,id,marker);if(operation==0)actual=child;else if(operation==1)actual=child.run(argument);else{Trace.arg=argument;Object value=((Callback)child.second).invoke(argument);Trace.after=value;Trace.hit(8);actual=value;}}catch(Throwable error){failure=error;}
   long events=0;boolean blocked=false,adapter=false,parent=false,written=false,child=false,copy=false,entered=false,invoked=false,after=false;int fail=0;
   if(mode==1){if(attempt==0){events=1;fail=1;}else fail=-1;blocked=true;}
   else if(mode==2){if(attempt==0){events=12;fail=2;}else fail=-1;blocked=true;}
   else {
    if(attempt==0)events=12;
    events=events*10+3;adapter=true;
    if(mode==3){fail=3;blocked=true;}
    else{events=events*10+4;parent=true;if(mode==4){fail=4;blocked=true;}
     else{written=true;events=events*10+5;if(mode==5){fail=5;blocked=true;}
      else{child=true;events=events*10+7;if(mode==7){fail=7;blocked=true;}
       else{copy=true;if(operation!=0){entered=true;if(operation==1){events=events*10+6;if(mode==6){fail=6;blocked=true;}}
        if(!blocked){invoked=true;events=events*10+9;if(mode==9){fail=9;blocked=true;}
         else if(operation==1&&result!=null&&!(result instanceof Token)){fail=-2;blocked=true;}
         else{after=true;events=events*10+8;if(mode==8){fail=8;blocked=true;}}}
       }}
      }
     }
    }
   }
   require(Trace.events==events&&(Trace.adapter!=null)==adapter&&(Trace.parent!=null)==parent&&(Trace.child!=null)==child);
   if(parent){require(Trace.parent.id==(written?id:0)&&Trace.parent.marker==(written?marker:null)&&Trace.parent.first==null&&Trace.parent.second==(written?Trace.adapter:null));require(((Child)Trace.parent).copy==(copy?id:0));}
   require(Trace.arg==(entered?argument:sentinel)&&Trace.seen==(invoked?argument:sentinel)&&Trace.after==(after?result:sentinel));
   if(fail==-1)require(failure instanceof NoClassDefFoundError);
   else if(fail==-2)require(failure instanceof ClassCastException);
   else if(fail==1||fail==2)require(failure instanceof ExceptionInInitializerError&&failure.getCause()==exact);
   else if(fail!=0)require(failure==exact);
   else require(failure==null&&actual==(operation==0?Trace.child:result));
   System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",events="+Trace.events);
  }
 }
}
