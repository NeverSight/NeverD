import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class PrivateBinaryCarrierHarness {
    static void require(boolean value){if(!value)throw new AssertionError("owned private binary carrier");}
    static Carrier call(int route,Provider peer,Carrier left,Carrier right) throws Throwable {
        if(route==0)return Provider.call(peer,left,right);
        if(route==1)return PrivateBinaryCarrierCaller.call(peer,left,right);
        if(route==2)try{return (Carrier)Provider.class.getMethod("call",Provider.class,Carrier.class,Carrier.class).invoke(null,peer,left,right);}catch(InvocationTargetException e){throw e.getCause();}
        return (Carrier)MethodHandles.publicLookup().findStatic(Provider.class,"call",MethodType.methodType(Carrier.class,Provider.class,Carrier.class,Carrier.class)).invokeExact(peer,left,right);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);boolean present=Boolean.parseBoolean(args[2]),isStatic=Boolean.parseBoolean(args[3]);
        Method read=Provider.class.getDeclaredMethod("read",Carrier.class,Carrier.class);
        require(Modifier.isPrivate(read.getModifiers())&&Modifier.isFinal(read.getModifiers())&&Modifier.isStatic(read.getModifiers())==isStatic);
        TypeVariable<?>[] formals=read.getTypeParameters();require(formals.length==1&&formals[0].getBounds().length==1&&formals[0].getBounds()[0]==Object.class);
        for(Type p:read.getGenericParameterTypes()) {require(p instanceof ParameterizedType);ParameterizedType t=(ParameterizedType)p;
            require(t.getRawType()==Carrier.class&&t.getActualTypeArguments().length==1);Type a=t.getActualTypeArguments()[0];require(a instanceof WildcardType);
            WildcardType w=(WildcardType)a;require(w.getLowerBounds().length==0&&w.getUpperBounds().length==1&&w.getUpperBounds()[0].equals(formals[0]));}
        ParameterizedType resultType=(ParameterizedType)read.getGenericReturnType();require(resultType.getRawType()==Carrier.class&&resultType.getActualTypeArguments()[0].equals(formals[0]));
        require(Provider.class.getMethod("call",Provider.class,Carrier.class,Carrier.class).getTypeParameters().length==0);
        Trace.failAt=0;Trace.observer=null;Provider peer=present?new Provider():null;
        RuntimeException primary=new IllegalStateException("owned primary");Error observerError=new AssertionError("owned observer");
        for(int attempt=0;attempt<2;attempt++) {
            Object firstValue=new Object(),secondValue=new Object(),element=new Object();
            Carrier<Object> a=()->{Trace.hit(6);return firstValue;},b=()->{Trace.hit(7);return secondValue;};
            final Carrier left=mode==11||mode==13?null:a,right=mode==12||mode==13?null:b;
            Carrier<Object> initial=()->{Trace.hit(8);return element;},later=()->{Trace.hit(9);return "later";};
            Trace.events=0;Trace.calls=0;Trace.receiver=null;Trace.left=null;Trace.right=null;Trace.first=null;Trace.second=null;
            Trace.result=mode==10?new Object():initial;Trace.failure=primary;Trace.failAt=mode>=1&&mode<=7?mode:0;
            Trace.observer=event->{
                if(event==2)require(Trace.receiver==(isStatic?null:peer)&&Trace.left==left&&Trace.right==right&&Trace.calls==1);
                if(event==3)require(Trace.first==(left==null?null:firstValue)&&Trace.second==null);
                if(event==4){require(Trace.first==(left==null?null:firstValue)&&Trace.second==(right==null?null:secondValue));if(mode==8)Trace.result=later;if(mode==14)throw observerError;}
                if(event==5&&mode==9)Trace.result=later;
            };
            Object result=null;Throwable failure=null;try{result=call(route,peer,left,right);if(mode==15)PrivateBinaryCarrierCaller.element((Carrier)result);}catch(Throwable e){failure=e;}
            long events=!present&&attempt==0?1:0;boolean entered=false,firstStored=false,secondStored=false;Throwable exact=null;boolean classFailure=!present&&mode==1,nullReceiver=!isStatic&&!present;
            if(classFailure){if(attempt==0)require(failure instanceof ExceptionInInitializerError&&failure.getCause()==primary);else require(failure instanceof NoClassDefFoundError);}
            else if(nullReceiver)require(failure instanceof NullPointerException);
            else {
                int[] sequence={2,left==null?0:6,3,right==null?0:7,4,5};boolean stopped=false;
                for(int event:sequence){if(event==0)continue;events=events*10+event;if(event==2)entered=true;if(event==3)firstStored=true;if(event==4)secondStored=true;
                    if(event==4&&mode==14)exact=observerError;else if(event==Trace.failAt)exact=primary;
                    if(exact!=null){stopped=true;break;}if(event==4&&mode==10){stopped=true;break;}}
                if(!stopped&&mode==15){events=events*10+8;require(failure instanceof ClassCastException);}
                else if(mode==10)require(failure instanceof ClassCastException);
                else require(failure==exact);
                if(!stopped)require(result==(mode==8?later:initial));else require(result==null);
            }
            require(Trace.events==events&&Trace.calls==(entered?1:0));
            require(Trace.first==(firstStored&&left!=null?firstValue:null)&&Trace.second==(secondStored&&right!=null?secondValue:null));
            if(entered)require(Trace.receiver==(isStatic?null:peer)&&Trace.left==left&&Trace.right==right);
            System.out.println("route="+route+",mode="+mode+",peer="+present+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
