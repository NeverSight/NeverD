import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class FilledIntConstructorHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned filled int constructor"); }
    static Core call(int route,int first,int second,Hook hook) throws Throwable {
        if(route==0)return new Core(first,second,hook);
        if(route==1)return FilledIntConstructorCaller.call(first,second,hook);
        if(route==2)try{return Core.class.getConstructor(int.class,int.class,Hook.class).newInstance(first,second,hook);}catch(InvocationTargetException e){throw e.getCause();}
        return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,int.class,int.class,Hook.class)).invokeExact(first,second,hook);
    }
    static void metadata() throws Throwable {
        Class<?> c=Class.forName("fixture.Core",false,FilledIntConstructorHarness.class.getClassLoader());
        require(c.getTypeParameters().length==0 && Modifier.isFinal(c.getModifiers()) && c.getDeclaredConstructors().length==2);
        for(String name:new String[]{"left","right"}) {
            Field f=c.getField(name);require(f.getType()==int[].class && f.getGenericType()==int[].class && Modifier.isFinal(f.getModifiers()));
        }
        for(Constructor<?> ctor:c.getDeclaredConstructors())require(ctor.getTypeParameters().length==0 && ctor.getExceptionTypes().length==0 && !ctor.isSynthetic());
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);boolean shared=Boolean.parseBoolean(args[2]);metadata();
        RuntimeException failure=new IllegalArgumentException("owned filled constructor failure");Trace.failure=failure;
        for(int attempt=0;attempt<2;attempt++) {
            int first=mode==10?Integer.MIN_VALUE:13+attempt,second=mode==10?Integer.MAX_VALUE:-5-attempt;
            Trace.events=0;Trace.partial=null;Trace.delta=mode==10?-1:7;Trace.post=91+attempt;
            Trace.failAt=mode==3?1:mode==4?2:mode==5?3:mode==6?4:mode==7?5:mode==8?6:mode==9?7:mode==12?8:mode==13?9:0;
            final int delta=Trace.delta,round=attempt,seed=first+delta;int[] calls={0};Object[] seen={null};int[][] seenRight={null};
            Hook observer=(value,right)->{
                Core core=(Core)value;calls[0]++;seen[0]=value;seenRight[0]=right;
                require(value==Trace.partial && core.left!=null && core.right==null && core.left.length==2 && right.length==2);
                require((core.left==right)==shared && core.left[0]==seed+delta && core.left[1]==second);
                require(right[0]==(shared?seed+delta:second) && right[1]==(shared?second:first));
                core.left[0]+=11;right[0]^=85;if(mode==11)Trace.post=Integer.MIN_VALUE+round;
                Trace.hit(8);if(mode==2)throw failure;
            };
            Core result=null;Throwable caught=null;
            try{result=call(route,first,second,mode==1?null:observer);}catch(Throwable e){caught=e;}
            int fail=Trace.failAt;long events=0;boolean reached=true;
            int[] steps={9,1,2,3,4,8,5,6,7};
            for(int step:steps) {
                if(step==9 && attempt==1)continue;
                if(mode==13 && attempt==1){reached=false;break;}
                if(step==8 && mode==1){reached=false;break;}
                events=events*10+step;
                if(step==fail || step==8 && mode==2){reached=false;break;}
            }
            require(Trace.events==events);
            if(mode==13)require(attempt==0?caught instanceof ExceptionInInitializerError && caught.getCause()==failure:caught instanceof NoClassDefFoundError);
            else if(mode==1)require(caught instanceof NullPointerException);
            else if(mode==2 || fail!=0)require(caught==failure);
            else require(caught==null && result==Trace.partial);
            boolean partial=mode!=3 && mode!=13,assigned=partial && mode!=4;
            require((Trace.partial!=null)==partial);
            Core core=(Core)Trace.partial;
            boolean invoked=assigned && mode!=5 && mode!=6 && mode!=1;
            require(calls[0]==(invoked?1:0));
            if(partial) {
                require((core.left!=null)==assigned);
                boolean rightAssigned=invoked && mode!=2 && mode!=7 && mode!=12;
                require((core.right!=null)==rightAssigned);
                if(assigned) {
                    int value=seed;if(mode!=5)value+=delta;
                    if(invoked){value+=11;if(shared)value^=85;require(seen[0]==core && (seenRight[0]==core.left)==shared);}
                    require(core.left[0]==value);
                    boolean post=rightAssigned && mode!=8;
                    require(core.left[1]==(post?(mode==11?Integer.MIN_VALUE+attempt:91+attempt):second));
                    if(invoked && !shared)require(seenRight[0][0]==(second^85) && seenRight[0][1]==first);
                    if(rightAssigned)require(core.right==seenRight[0]);
                }
            }
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
