import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class StaticMethodEnclosureHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("owned static enclosure contract"); }
    static Object call(int route, Object input, Object marker, boolean defaults) throws Throwable {
        if (route==0) return defaults ? Provider.relay(input) : Provider.provided(input,marker);
        if (route==1) return StaticMethodEnclosureCaller.call(input,marker,defaults);
        if (route==2) {
            try { return defaults ? Provider.class.getMethod("relay",Object.class).invoke(null,input)
                : Provider.class.getMethod("provided",Object.class,Object.class).invoke(null,input,marker); }
            catch(InvocationTargetException failure){throw failure.getCause();}
        }
        MethodHandles.Lookup lookup=MethodHandles.publicLookup();
        return defaults ? lookup.findStatic(Provider.class,"relay",MethodType.methodType(Object.class,Object.class)).invoke(input)
            : lookup.findStatic(Provider.class,"provided",MethodType.methodType(Object.class,Object.class,Object.class)).invoke(input,marker);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]), variant=Integer.parseInt(args[1]), mode=Integer.parseInt(args[2]);
        String helperName=args[3]; boolean defaults=variant==0;
        Method helper=Provider.class.getDeclaredMethod(helperName,Object.class,Object.class,int.class,Object.class);
        Trace.failure=new IllegalArgumentException("owned enclosure failure");
        Trace.fail=mode>=1 && mode<=3 ? mode : mode==6 ? 9 : 0;
        int[] data={7}; Object input=mode==4 ? null : mode==5 ? new Object() : data;
        Object explicit=mode==7 ? null : new Object(), oldMarker=null;
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0; Trace.calls=0; Object prior=new Object(); Trace.result=prior; Trace.marker=prior;
            int oldData=data[0]; Object result=new Object(); Throwable failure=null;
            try { result=call(route,input,explicit,defaults); } catch(Throwable caught){failure=caught;}
            boolean entered=mode!=1 && mode!=6;
            boolean stored=entered && mode!=2 && mode!=5;
            long expected=mode==6 ? (attempt==0?9:0) : stored?123:entered?12:1;
            if(mode!=6 && attempt==0) expected=stored?9123:entered?912:91;
            if(mode>=1 && mode<=3) require(failure==Trace.failure);
            else if(mode==5) require(failure instanceof ClassCastException);
            else if(mode==6) require(attempt==0 ? failure instanceof ExceptionInInitializerError && failure.getCause()==Trace.failure : failure instanceof NoClassDefFoundError);
            else require(failure==null && result==input);
            require(Trace.events==expected && Trace.calls==(entered?1:0));
            require(Trace.result==(stored?input:prior) && data[0]==oldData+(stored && input==data?1:0));
            if(mode!=6) {
                if(defaults) {
                    require(Trace.marker!=null && Trace.marker!=oldMarker && helper.equals(Trace.marker.getClass().getEnclosingMethod()));
                    require(Trace.marker.getClass().isAnonymousClass()==args[4].equals("input"));
                    oldMarker=Trace.marker;
                } else require(Trace.marker==explicit);
            } else require(Trace.marker==prior);
            System.out.println("route="+route+",variant="+variant+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
