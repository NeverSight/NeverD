import fixture.*;
import java.util.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ComparisonCarrierHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned comparison carrier"); }
    static void metadata(boolean bounded,boolean packageOwner) throws Throwable {
        Class<?> provider=Class.forName("fixture.Provider",false,Client.class.getClassLoader());
        require(Modifier.isPublic(provider.getModifiers())!=packageOwner);
        TypeVariable<?> owner=provider.getTypeParameters()[0];
        require(owner.getName().equals("T") && owner.getBounds()[0]==(bounded?Number.class:Object.class));
        Method method=provider.getDeclaredMethod("scan",List.class,int.class,int.class,Comparison.class);
        require(Modifier.isStatic(method.getModifiers()) && method.getGenericReturnType()==int.class);
        require(method.getTypeParameters().length==1);
        TypeVariable<?> formal=method.getTypeParameters()[0];
        require(formal.getName().equals("T") && formal.getGenericDeclaration().equals(method));
        require(formal.getBounds()[0]==Object.class && !formal.equals(owner));
        Type[] types=method.getGenericParameterTypes();require(types.length==4 && types[1]==int.class && types[2]==int.class);
        ParameterizedType values=(ParameterizedType)types[0],callback=(ParameterizedType)types[3];
        require(values.getRawType()==List.class && callback.getRawType()==Comparison.class);
        WildcardType produced=(WildcardType)values.getActualTypeArguments()[0],consumed=(WildcardType)callback.getActualTypeArguments()[0];
        require(produced.getLowerBounds().length==0 && produced.getUpperBounds()[0].equals(formal));
        require(consumed.getLowerBounds().length==1 && consumed.getLowerBounds()[0].equals(formal) && consumed.getUpperBounds()[0]==Object.class);
        require(callback.getActualTypeArguments()[1]==Integer.class);
    }
    static int call(int route,List values,int from,int to,Comparison comparison) throws Throwable {
        if(route==0)return Client.call(values,from,to,comparison);
        if(route==1)return ComparisonCarrierCaller.call(values,from,to,comparison);
        if(route==2)try{return (Integer)Client.class.getMethod("call",List.class,int.class,int.class,Comparison.class).invoke(null,values,from,to,comparison);}catch(InvocationTargetException error){throw error.getCause();}
        return (int)MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(int.class,List.class,int.class,int.class,Comparison.class)).invokeExact(values,from,to,comparison);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);metadata(Boolean.parseBoolean(args[2]),Boolean.parseBoolean(args[3]));
        RuntimeException failure=new IllegalArgumentException("owned comparison failure");Trace.failure=failure;
        for(int attempt=0;attempt<2;attempt++) {
            Object first=mode==10?null:new String("owned value"),second=new int[]{9},replacement=new Object(),sentinel=new Object();
            List values=mode==1?null:new ArrayList(Arrays.asList(new Object(),first,second));
            final List backing=values;final int round=attempt;int[] calls={0};
            Trace.events=0;Trace.sum=0;Trace.current=sentinel;
            Trace.result=mode==11?Integer.MIN_VALUE:0x1234567+attempt;int initial=Trace.result;
            Trace.failAt=mode==12?1:mode==13?3:mode==14?5:mode==15?6:0;
            Comparison<Object,Object> hook=value->{
                int at=calls[0]++;require(at<2 && value==(at==0?first:mode==9?replacement:second) && Trace.current==value);
                Trace.hit(4);if(mode==4)throw failure;
                if(mode==9 && at==0)backing.set(2,replacement);
                if(mode==17 && at==0)Trace.result=73+round;
                if(mode==3)return null;if(mode==16)return new Object();
                return Integer.valueOf(mode==11?(at==0?Integer.MIN_VALUE:Integer.MAX_VALUE):(at==0?11:-7));
            };
            Comparison callback=mode==2?null:hook;
            int from=mode==1?0:mode==5?-1:mode==7?2:mode==8?3:1;
            int to=mode==1?1:mode==5?1:mode==6?4:mode==7?2:mode==8?1:3;
            int result=0;Throwable caught=null;try{result=call(route,values,from,to,callback);}catch(Throwable error){caught=error;}
            int[] sequence=mode==12?(attempt==0?new int[]{1}:new int[]{})
                :mode==1||mode==5?new int[]{1,2}
                :mode==7||mode==8?new int[]{1,2,6}
                :mode==2||mode==13?new int[]{1,2,3}
                :mode==3||mode==4||mode==16?new int[]{1,2,3,4}
                :mode==14?new int[]{1,2,3,4,5}
                :mode==6?new int[]{1,2,3,4,5,3,4,5}:new int[]{1,2,3,4,5,3,4,5,6};
            long events=0;for(int event:sequence)if(!(attempt==1 && event==1))events=events*10+event;
            require(Trace.events==events);
            if(mode==12)require(attempt==0?caught instanceof ExceptionInInitializerError&&caught.getCause()==failure:caught instanceof NoClassDefFoundError);
            else if(mode==1||mode==2||mode==3)require(caught instanceof NullPointerException);
            else if(mode==4||mode==13||mode==14||mode==15)require(caught==failure);
            else if(mode==5||mode==6)require(caught instanceof IndexOutOfBoundsException);
            else if(mode==16)require(caught instanceof ClassCastException);
            else require(caught==null && result==(mode==17?73+attempt:initial));
            int count=mode==1||mode==2||mode==5||mode==7||mode==8||mode==12||mode==13?0
                :mode==3||mode==4||mode==14||mode==16?1:2;
            require(calls[0]==count);
            boolean noRead=mode==1||mode==5||mode==7||mode==8||mode==12;
            boolean oneRead=mode==2||mode==3||mode==4||mode==13||mode==14||mode==16;
            require(Trace.current==(noRead?sentinel:oneRead?first:mode==9?replacement:second));
            int committed=mode==1||mode==2||mode==3||mode==4||mode==5||mode==7||mode==8||mode==12||mode==13||mode==16?0:mode==14?1:2;
            int sum=committed==0?0:committed==1?11:mode==11?-1:4;
            require(Trace.sum==sum && Trace.result==(mode==17?73+attempt:initial));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
