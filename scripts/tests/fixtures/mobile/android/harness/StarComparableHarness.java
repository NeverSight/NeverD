import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class StarComparableHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned star comparison"); }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Method comparison=Comparisons.class.getMethod("compareValues",Comparable.class,Comparable.class);
        TypeVariable<?> formal=comparison.getTypeParameters()[0];ParameterizedType bound=(ParameterizedType)formal.getBounds()[0];
        require(bound.getRawType()==Comparable.class&&((WildcardType)bound.getActualTypeArguments()[0]).getLowerBounds().length==0);
        require(((WildcardType)bound.getActualTypeArguments()[0]).getUpperBounds()[0]==Object.class);
        require(comparison.getGenericParameterTypes()[0].equals(formal)&&comparison.getGenericParameterTypes()[1].equals(formal));
        Trace.failure=new RuntimeException("owned comparison failure");Trace.failAt=mode==1?5:mode==2?7:mode==3?1:mode==4?4:mode==5?6:0;
        Object sentinel=new Object();Node a=new Node(3),b=new Node(7);
        Comparable first=mode==6||mode==7?null:a;
        Comparable second=mode==6||mode==8?null:mode==9?a:mode==10?(Comparable<Object>)(o)->42:b;
        for(int attempt=0;attempt<2;attempt++) {
            if(attempt!=0) {a.key=11;b.key=4;}
            Trace.events=0;Trace.receiver=Trace.other=sentinel;Trace.result=12345;
            int before=Trace.calls;Throwable failure=null;int result=12345;
            try {
                if(route==0)result=Core.call(first,second);
                else if(route==1)result=StarComparableCaller.call(first,second);
                else if(route==2)result=(Integer)Core.class.getMethod("call",Comparable.class,Comparable.class).invoke(null,first,second);
                else result=(int)MethodHandles.publicLookup().findStatic(Core.class,"call",MethodType.methodType(int.class,Comparable.class,Comparable.class)).invoke(first,second);
            } catch(InvocationTargetException error) { failure=error.getCause(); }
              catch(Throwable error) { failure=error; }
            String steps="5";
            if(mode!=1&&!(mode==2&&attempt!=0)) {
                if(attempt==0)steps+="7";
                if(mode!=2) {
                    steps+="1";
                    if(mode!=3) {
                        if(mode<6||mode==10)steps+="2";
                        if(mode<6)steps+="4";
                        if(mode!=4&&mode!=10)steps+="6";
                    }
                }
            }
            require(Trace.events==Long.parseLong(steps));boolean called=steps.indexOf('4')>=0;
            require(Trace.calls==before+(called?1:0)&&Trace.receiver==(called?first:sentinel)&&Trace.other==(called?second:sentinel));
            int expected=mode==6||mode==9?0:mode==7?-1:mode==8?1:a.key-b.key;
            require(Trace.result==(steps.indexOf('6')>=0?expected:12345));
            if(mode==1||mode==3||mode==4||mode==5)require(failure==Trace.failure);
            else if(mode==2)require(attempt==0?failure instanceof ExceptionInInitializerError&&failure.getCause()==Trace.failure:failure instanceof NoClassDefFoundError);
            else if(mode==10)require(failure instanceof ClassCastException);
            else require(failure==null&&result==expected);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
