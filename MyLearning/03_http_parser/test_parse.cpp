#include<iostream>
#include<string.h>
#include<vector>

using namespace std;

int main(){
    // 1. 模拟浏览器发来的一坨数据
    // 注意：\r\n 是换行符
    char buffer[]=  "GET /index.html HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";

    printf("原始数据:\n%s\n", buffer);
    printf("--------------------------------\n");

    // 2. 准备开始切菜
    // 指针 p 指向数据的开头
    char *p=buffer;
    
    // 3. 这里的逻辑就是“第一板斧”的简化版：用 strstr 找 \r\n
    // strstr(A, B) 会在 A 里找 B，返回 B 第一次出现的位置
    char *end_of_line;

    int line_count=1;

    // 只要还能找到 \r\n，就一直切。    这个while循环要重点理解！！🤓
    while((end_of_line=strstr(p,"\r\n"))!=NULL){//为了零拷贝，然后end_of_line和p用buffer的同一块内存！

        // 4. 【关键动作】把 \r\n 变成 \0 (字符串结束符)。打印字符串 看到 \0 才会停止打印
        // 这样 p 指向的字符串就会在 end_of_line 这里截断
        *end_of_line='\0';

        // 5. 打印切出来的这一行
        printf("第 %d 行切出来的: %s\n", line_count++, p);

        // 6. 移动指针 p，跳过刚才的 \r\n (2个字符)，准备切下一行
        p=end_of_line+2;
    }

    return 0;

}   