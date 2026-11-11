import fixture.*;
import java.lang.reflect.*;
public final class StaticMemberTablesHarness {
    static void check(boolean value){if(!value)throw new AssertionError("owned static member table mismatch");}
    public static void main(String[] args)throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]),stage=Integer.parseInt(args[2]);
        check(Modifier.isStatic(Host.Table.class.getModifiers())==(stage==0));
        check(Host.Table.class.getDeclaredConstructors().length==(stage==1?0:1));
        Trace.provided=mode==1?null:new int[]{20};Trace.late=new int[]{900};
        Seed.next=mode==10?new Object():Trace.provided;Seed.fail=mode==2;Seed.failure=new RuntimeException("owned seed failure");
        Trace.failAt=mode==3?1:mode==4?2:mode==5?3:mode==6?4:mode==7?5:-1;
        Trace.failure=new RuntimeException("owned trace failure");
        int index=mode==8?2:mode==9?-1:0;
        boolean initFailure=mode>=2&&mode<=5||mode==10;
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Integer answer=null;Throwable caught=null;
            int before=Trace.provided==null?0:Trace.provided[0];
            try {if(route==0)answer=Host.run(index);else Host.ignore(index);}catch(Throwable failure){caught=failure;}
            if(initFailure) {
                if(attempt==0){check(caught instanceof ExceptionInInitializerError);Throwable cause=caught.getCause();check(mode==2?cause==Seed.failure:mode==10?cause instanceof ClassCastException:cause==Trace.failure);}
                else check(caught instanceof NoClassDefFoundError);
                long expected=attempt==1?0:mode==3?1:mode==2||mode==10?16:mode==4?162:1623;
                check(Trace.events==expected);
            } else {
                boolean failure=mode==1||mode==6||mode==7||mode==8||mode==9;
                if(mode==1)check(caught instanceof NullPointerException);
                else if(mode==6||mode==7)check(caught==Trace.failure);
                else if(mode==8||mode==9)check(caught instanceof ArrayIndexOutOfBoundsException);
                else check(caught==null&&(route==1||answer.intValue()==before+10));
                long suffix=mode==1||mode==6||mode==8||mode==9?4:mode==7?45:459;
                long expected=attempt==0?(suffix==4?16234:suffix==45?162345:1623459):suffix;
                check(Trace.events==expected);check(failure||Seed.next==Trace.late);
            }
        }
        System.out.println("attempts=2,static-member-table=matched");
    }
}
