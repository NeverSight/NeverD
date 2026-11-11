import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class SplitSeedFactoryHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("owned split seed factory"); }
    static Carrier call(int route, Carrier parent, Object first, Object second, String label, int mode) throws Throwable {
        try {
            if (route == 0) return Client.call(parent,first,second,label,mode);
            if (route == 1) return SplitSeedFactoryCaller.call(parent,first,second,label,mode);
            if (route == 2) return (Carrier)Provider.class.getMethod("create",Carrier.class,Object.class,Object.class,String.class,int.class)
                .invoke(null,parent,first,second,label,mode);
            return (Carrier)MethodHandles.publicLookup().findStatic(Provider.class,"create",
                MethodType.methodType(Carrier.class,Carrier.class,Object.class,Object.class,String.class,int.class))
                .invokeExact(parent,first,second,label,mode);
        } catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]), mode=Integer.parseInt(args[1]);
        Method factory=Provider.class.getMethod("create",Carrier.class,Object.class,Object.class,String.class,int.class);
        TypeVariable<?>[] formals=factory.getTypeParameters();
        require(formals.length==2 && factory.getGenericParameterTypes()[1].equals(formals[1])
            && factory.getGenericParameterTypes()[2].equals(formals[1]));
        require(((ParameterizedType)factory.getGenericParameterTypes()[0]).getActualTypeArguments()[0].equals(formals[0]));
        require(((ParameterizedType)factory.getGenericReturnType()).getActualTypeArguments()[0].equals(formals[1]));
        int[] failures={0,0,0,0,1,2,4,5,6,3,0,0,0,0};
        RuntimeException original=new IllegalArgumentException("owned split failure");
        Carrier previous=null;
        for(int attempt=0;attempt<2;attempt++) {
            Trace.failAt=0;
            Object parentValue=new Object(),otherFirst=new Object(),otherSecond=new Object();
            Carrier parent=new Carrier<Object>(parentValue,new Object());
            Trace.other=new Carrier<Object>(otherFirst,otherSecond);
            Object first=mode==1?null:new Object(),second=mode==2?null:new Object();
            String label=mode==12?null:"owned label";
            int kind=mode==10?1:mode==11?2:mode==13?3:0;
            Trace.events=0;Trace.parent=Trace.created=null;Trace.captured=Trace.extracted=null;Trace.label=null;
            Trace.failAt=failures[mode];Trace.failure=original;
            boolean failing=Trace.failAt!=0 || mode==3;
            Carrier actual=null;
            try {
                actual=call(route,mode==3?null:parent,first,second,label,kind);
                require(!failing);
            } catch(Throwable failure) {
                require(failing && (mode==3?failure instanceof NullPointerException:failure==original));
            }
            long expected=mode==3||mode==4?1:mode==5||kind!=0?12:mode==6?124:mode==7?1245:mode==8?12456:124563;
            require(Trace.events==expected);
            if(mode!=4)require(Trace.parent==(mode==3?null:parent));
            if(mode!=3&&mode!=4)require(Trace.extracted==parentValue&&Trace.label==label);
            if(mode>=6&&mode<=9 || !failing&&kind==0) {
                require(Trace.captured instanceof Carrier);
                Carrier captured=(Carrier)Trace.captured;
                require(captured.first()==(mode==6?null:first));
                require(captured.second()==(mode==6||mode==7?null:second));
                require(Trace.created==(mode>=6&&mode<=8?null:captured));
                if(!failing)require(actual==captured&&actual!=previous);
            } else require(Trace.captured==null&&Trace.created==null);
            if(!failing&&kind==1)require(actual==Trace.other&&actual.first()==otherFirst&&actual.second()==otherSecond);
            if(!failing&&kind==2)require(actual==null);
            if(!failing&&kind==3)require(actual==parent);
            previous=actual;
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
